/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* capture_linux_wifi
 *
 * Capture binary, written in pure c, which interfaces via the Kismet capture
 * protocol and feeds packets from, and is able to control, a wireless card on 
 * Linux, using either the old iwconfig IOCTL interface (deprecated) or the
 * modern nl80211 netlink interface.
 *
 * The communications channel is a file descriptor pair, passed via command
 * line arguments, --in-fd= and --out-fd=
 *
 * We parse additional options from the source definition itself, such as a DLT
 * override, once we open the protocol
 *
 * The packets undergo as little processing as possible and are passed to Kismet
 * to process the DLT.
 *
 * This binary needs to run as root to be able to control and capture from
 * the interface - and it needs to continue running as root to be able to control
 * the channels.
 *
 */

#include <pcap.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>

/* According to POSIX.1-2001, POSIX.1-2008 */
#include <sys/select.h>

/* According to earlier standards */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <unistd.h>
#include <errno.h>

#include <net/if.h>
#include <arpa/inet.h>

#include <ifaddrs.h>

#include "config.h"

#ifdef HAVE_LIBNM
#include <libnm/NetworkManager.h>
#include <glib.h>
#endif

#include "simple_datasource_proto.h"
#include "capture_framework.h"

#include "interface_control.h"
#include "linux_wireless_control.h"
#include "linux_netlink_control.h"

#include "wifi_ht_channels.h"

#define MAX_PACKET_LEN  8192

/* State tracking, put in userdata */
typedef struct {
    pcap_t *pd;

    char *interface;
    char *cap_interface;

    int datalink_type;
    int override_dlt;

    /* Do we use mac80211 controls or basic ioctls */
    int use_mac80211;

    /* Cached mac80211 controls */
    void *mac80211_handle;
    void *mac80211_cache;
    void *mac80211_family;

    /* Number of sequential errors setting channel */
    unsigned int seq_channel_failure;

    /* Do we try to reset networkmanager when we're done? */
    int reset_nm_management;
} local_wifi_t;

/* Linux Wi-Fi Channels:
 *
 * Wi-Fi can use multiple channel widths and encodings which need to be
 * accounted for.
 *
 * Channel formats:
 *
 * XXW5         Channel/frequency XX, custom 5MHz channel
 * XXW10        Channel/frequency XX, custom 10MHz channel
 * XX           Channel/frequency XX, non-HT standard 20MHz channel
 * XXHT40+      Channel/frequency XX, HT40+ channel
 * XXHT40-      Channel/frequency XX, HT40- channel
 * XXVHT80      Channel/frequency XX, VHT 80MHz channel.  Upper pair automatically
 *              derived from channel definition table
 * XXVHT160     Channel/frequency XX, VHT 160MHz channel.  Upper pair automatically
 *              derived from channel definition table
 *
 * XXVHT80-YY   Channel/frequency XX, VHT 80MHz channel, upper pair specified
 * XXVHT160-YY  Channel/frequency XX, VHT 160MHz channel, upper pair specified
 *
 * 5, 10, HT, and VHT channels require mac80211 drivers; the old wireless IOCTLs do
 * not support the needed attributes.
 */

/* Local interpretation of a channel; this lets us parse the string definitions
 * into a faster non-parsed version, once. */
typedef struct {
    /* For stock 20mhz channels, center freq is set to channel and 
     * chan_type is set to 0/NL80211_CHAN_NO_HT
     *
     * For ht40 channels we set only the center freq/chan and the type 
     * is set to NL80211_CHAN_HT40MINUS/HT40PLUS
     *
     * For vht80 and vht160, center freq is set, chan_type is set to 0,
     * chan_width is set accordingly to one of NL80211_CHAN_WIDTH_, and
     * center_freq1 is set to the corresponding vht center frequency.
     *
     * If 'unusual_center1' is true, the center_freq1 was not derived
     * automatically; this is relevant only when printing
     *
     * For sub-20mhz channels, chan_type is set to 0, chan_width is set 
     * accordingly from NL80211_CHAN_WIDTH_5/10, and center_freq1 is 0.
     */
    unsigned int control_freq;
    unsigned int chan_type;
    unsigned int chan_width;
    unsigned int unusual_center1;
    unsigned int center_freq1;
    unsigned int center_freq2;
} local_channel_t;

/* Find an interface based on a mac address (or mac address prefix in the case
 * of monitor mode interfaces); if we have to make a disassociated monitor interface
 * name we want to be able to find it again if we re-open
 *
 * if ignored_ifname is not null, we will ignore any interface which matches mac
 * but has the same name; we only want to find the monitor interface variant. 
 *
 * wlmode will typically be LINUX_WLEXT_MONITOR but could be any other wireless
 * extensions mode.
 *
 * returns the ifnum index which can then be resolved into an interface 
 * name.
 */
int find_interface_mode_by_mac(const char *ignored_ifname, int wlmode, uint8_t *mac) {
    struct ifaddrs *ifaddr, *ifa;
    uint8_t dmac[6];
    char errstr[STATUS_MAX];
    int r;

    if (getifaddrs(&ifaddr) == -1)
        return -1;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ignored_ifname != NULL && strcmp(ifa->ifa_name, ignored_ifname) == 0)
            continue;

        if (ifconfig_get_hwaddr(ifa->ifa_name, errstr, dmac) < 0)
            continue;

        if (memcmp(dmac, mac, 6) == 0) {
            /* Found matching mac addr, which doesn't match our existing name,
             * is it in the right mode? */
            int mode;

            if (iwconfig_get_mode(ifa->ifa_name, errstr, &mode) >= 0) {
                if (mode == wlmode) {
                    r = if_nametoindex(ifa->ifa_name);
                    freeifaddrs(ifaddr);
                    return r;
                }
            }
        }
    }

    return -1;
}

/* Find the next unused interface number for a given interface name */
int find_next_ifnum(const char *basename) {
    int i;
    char ifname[IFNAMSIZ];

    for (i = 0; i < 100; i++) {
        snprintf(ifname, IFNAMSIZ, "%s%d", basename, i);

        if (if_nametoindex(ifname) == 0)
            return i;
    }

    return -1;
}

/* Convert a string into a local interpretation; allocate ret_localchan.
 */
void *chantranslate_callback(kis_capture_handler_t *caph, char *chanstr) {
    local_channel_t *ret_localchan;
    unsigned int parsechan, parse_center1;
    char parsetype[16];
    char mod;
    int r;
    unsigned int ci;
    char errstr[STATUS_MAX];

    /* Match HT40+ and HT40- */
    r = sscanf(chanstr, "%uHT40%c", &parsechan, &mod);

    if (r == 2) {
        ret_localchan = (local_channel_t *) malloc(sizeof(local_channel_t));
        memset(ret_localchan, 0, sizeof(local_channel_t));

        (ret_localchan)->control_freq = parsechan;

        if (mod == '-') {
            (ret_localchan)->chan_type = NL80211_CHAN_HT40MINUS;

            /* Search for the ht channel record */
            for (ci = 0; ci < MAX_WIFI_HT_CHANNEL; ci++) {
                if (wifi_ht_channels[ci].chan == parsechan || 
                        wifi_ht_channels[ci].freq == parsechan) {

                    if ((wifi_ht_channels[ci].flags & WIFI_HT_HT40MINUS) == 0) {
                        snprintf(errstr, STATUS_MAX, "requested channel %u as a HT40- "
                                "channel; this does not appear to be a valid channel "
                                "for 40MHz operation.", parsechan);
                        cf_send_message(caph, errstr, MSGFLAG_INFO);
                    }

                }
            }
        } else if (mod == '+') {
            (ret_localchan)->chan_type = NL80211_CHAN_HT40PLUS;

            /* Search for the ht channel record */
            for (ci = 0; ci < sizeof(wifi_ht_channels) / 
                    sizeof (wifi_channel); ci++) {
                if (wifi_ht_channels[ci].chan == parsechan || 
                        wifi_ht_channels[ci].freq == parsechan) {

                    if ((wifi_ht_channels[ci].flags & WIFI_HT_HT40PLUS) == 0) {
                        snprintf(errstr, STATUS_MAX, "requested channel %u as a HT40+ "
                                "channel; this does not appear to be a valid channel "
                                "for 40MHz operation.", parsechan);
                        cf_send_message(caph, errstr, MSGFLAG_INFO);
                    }

                }
            }
        } else {
            /* otherwise return it as a basic channel; we don't know what to do */
            snprintf(errstr, STATUS_MAX, "unable to parse attributes on channel "
                    "'%s', treating as standard non-HT channel.", chanstr);
            cf_send_message(caph, errstr, MSGFLAG_INFO);
        }

        return ret_localchan;
    }


    /* otherwise parse VHTXX, WXX, and VHTXX-YYY */
    r = sscanf(chanstr, "%u%15[^-]-%u", &parsechan, parsetype, &parse_center1);

    if (r <= 0) {
        snprintf(errstr, STATUS_MAX, "unable to parse any channel information from "
                "channel string '%s'", chanstr);
        cf_send_message(caph, errstr, MSGFLAG_ERROR);
        fprintf(stderr, "debug - %s\n", errstr);
        return NULL;
    }

    ret_localchan = (local_channel_t *) malloc(sizeof(local_channel_t));
    memset(ret_localchan, 0, sizeof(local_channel_t));

    if (r == 1) {
        (ret_localchan)->control_freq = parsechan;
        return ret_localchan;
    }

    if (r >= 2) {
        (ret_localchan)->control_freq = parsechan;

        if (strcasecmp(parsetype, "w5") == 0) {
            (ret_localchan)->chan_width = NL80211_CHAN_WIDTH_5;
        } else if (strcasecmp(parsetype, "w10") == 0) {
            (ret_localchan)->chan_width = NL80211_CHAN_WIDTH_10;
        } else if (strcasecmp(parsetype, "vht80") == 0) {
            (ret_localchan)->chan_width = NL80211_CHAN_WIDTH_80;

            /* Do we have a hardcoded 80mhz freq pair? */
            if (r == 3) {
                (ret_localchan)->center_freq1 = parse_center1;
                (ret_localchan)->unusual_center1 = 1;
            } else {
                /* Search for the vht channel record to find the 80mhz center freq */
                for (ci = 0; ci < sizeof(wifi_ht_channels) / 
                        sizeof (wifi_channel); ci++) {
                    if (wifi_ht_channels[ci].chan == parsechan || 
                            wifi_ht_channels[ci].freq == parsechan) {

                        if ((wifi_ht_channels[ci].flags & WIFI_HT_HT80) == 0) {
                            snprintf(errstr, STATUS_MAX, "requested channel %u as a "
                                    "VHT80 channel; this does not appear to be a valid "
                                    "channel for 80MHz operation, skipping channel", 
                                    parsechan);
                            cf_send_message(caph, errstr, MSGFLAG_ERROR);
                            free(ret_localchan);
                            return NULL;
                        }

                        (ret_localchan)->control_freq = wifi_ht_channels[ci].freq;
                        (ret_localchan)->center_freq1 = wifi_ht_channels[ci].freq80;
                    }
                }
            }
        } else if (strcasecmp(parsetype, "vht160") == 0) {
            (ret_localchan)->chan_width = NL80211_CHAN_WIDTH_160;

            /* Do we have a hardcoded 80mhz freq pair? */
            if (r == 3) {
                (ret_localchan)->center_freq1 = parse_center1;
                (ret_localchan)->unusual_center1 = 1;
            } else {
                /* Search for the vht channel record to find the 160mhz center freq */
                for (ci = 0; ci < sizeof(wifi_ht_channels) / 
                        sizeof (wifi_channel); ci++) {
                    if (wifi_ht_channels[ci].chan == parsechan || 
                            wifi_ht_channels[ci].freq == parsechan) {

                        if ((wifi_ht_channels[ci].flags & WIFI_HT_HT160) == 0) {
                            snprintf(errstr, STATUS_MAX, "requested channel %u as a "
                                    "VHT160 channel; this does not appear to be a "
                                    "valid channel for 160MHz operation, skipping "
                                    "channel", parsechan);
                            cf_send_message(caph, errstr, MSGFLAG_ERROR);
                            free(ret_localchan);
                            return NULL;
                        }

                        (ret_localchan)->control_freq = wifi_ht_channels[ci].freq;
                        (ret_localchan)->center_freq1 = wifi_ht_channels[ci].freq160;
                    }
                }
            }
        } else {
            /* otherwise return it as a basic channel; we don't know what to do */
            snprintf(errstr, STATUS_MAX, "unable to parse attributes on channel "
                    "'%s', treating as standard non-HT channel.", chanstr);
            cf_send_message(caph, errstr, MSGFLAG_INFO);
        }

    }

    return ret_localchan;
}

/* Convert a local interpretation of a channel back info a string;
 * 'chanstr' should hold at least STATUS_MAX characters; we'll never use
 * that many but it lets us do some cheaty stuff and re-use errstrs */
void local_channel_to_str(local_channel_t *chan, char *chanstr) {
    /* Basic channel with no HT/VHT */
    if (chan->chan_type == 0 && chan->chan_width == 0) {
        snprintf(chanstr, STATUS_MAX, "%u", chan->control_freq);
    } else if (chan->chan_type == NL80211_CHAN_HT40MINUS) {
        snprintf(chanstr, STATUS_MAX, "%uHT40-", chan->control_freq);
    } else if (chan->chan_type == NL80211_CHAN_HT40PLUS) {
        snprintf(chanstr, STATUS_MAX, "%uHT40+", chan->control_freq);
    } else {
        /* We've got some channel width; work with them */
        switch (chan->chan_width) {
            case NL80211_CHAN_WIDTH_5:
                snprintf(chanstr, STATUS_MAX, "%uW5", chan->control_freq);
                break;
            case NL80211_CHAN_WIDTH_10:
                snprintf(chanstr, STATUS_MAX, "%uW10", chan->control_freq);
                break;
            case NL80211_CHAN_WIDTH_80:
                if (chan->unusual_center1) {
                    snprintf(chanstr, STATUS_MAX, "%uVHT80-%u",
                            chan->control_freq, chan->center_freq1);
                } else {
                    snprintf(chanstr, STATUS_MAX, "%uVHT80", chan->control_freq);
                }
                break;
            case NL80211_CHAN_WIDTH_160:
                if (chan->unusual_center1) {
                    snprintf(chanstr, STATUS_MAX, "%uVHT160-%u",
                            chan->control_freq, chan->center_freq1);
                } else {
                    snprintf(chanstr, STATUS_MAX, "%uVHT160", chan->control_freq);
                }
                break;
            default:
                /* Just put the basic freq if we can't figure out what to do */
                snprintf(chanstr, STATUS_MAX, "%u", chan->control_freq);
                break;
        }
    }
}

int populate_chanlist(char *interface, char *msg, char ***chanlist, 
        size_t *chanlist_sz) {
    int ret;
    unsigned int *iw_chanlist;
    unsigned int chan_sz;
    unsigned int ci;
    char conv_chan[16];

    /* Prefer mac80211 channel fetch */
    ret = mac80211_get_chanlist(interface, msg, chanlist, 
            (unsigned int *) chanlist_sz);

    if (ret < 0) {
        ret = iwconfig_get_chanlist(interface, msg, &iw_chanlist, &chan_sz);

        /* We can't seem to get any channels from this interface, either 
         * through mac80211 or siocgiwfreq so we can't do anything */
        if (ret < 0 || chan_sz == 0) {
            return 0;
        }

        *chanlist = (char **) malloc(sizeof(char *) * chan_sz);

        for (ci = 0; ci < chan_sz; ci++) {
            snprintf(conv_chan, 16, "%u", iw_chanlist[ci]);
            (*chanlist)[ci] = strdup(conv_chan);
        }
        
        free(iw_chanlist);

        *chanlist_sz = chan_sz;
    } else {
        /*
        fprintf(stderr, "debug - linux wifi %s got channel list: \n", interface);
        for (unsigned int i = 0; i < *chanlist_sz; i++) {
            fprintf(stderr, "debug -     %s\n", (*chanlist)[i]);
        }
        */
    }

    return 1;
}

int probe_callback(kis_capture_handler_t *caph, uint32_t seqno, char *definition,
        char *msg, char **chanset, char ***chanlist, size_t *chanlist_sz) {
    char *placeholder = NULL;
    int placeholder_len;
    char *interface;
    int ret;

    *chanset = NULL;
    *chanlist = NULL;
    *chanlist_sz = 0;

    if ((placeholder_len = cf_parse_interface(&placeholder, definition)) <= 0) {
        snprintf(msg, STATUS_MAX, "Unable to find interface in definition"); 
        return 0;
    }

    interface = strndup(placeholder, placeholder_len);

    /* We don't care about fixed channel */
    *chanset = NULL;
   
    ret = populate_chanlist(interface, msg, chanlist, chanlist_sz);

    free(interface);

    if (ret < 0)
        return -1;

    return 1;
}

int open_callback(kis_capture_handler_t *caph, uint32_t seqno, char *definition,
        char *msg, char **uuid, char **chanset, char ***chanlist, size_t *chanlist_sz) {
    /* Try to open an interface for monitoring
     * 
     * - Confirm it's an interface, and that it's wireless, by doing a basic 
     *   siocgiwchan channel fetch to see if wireless icotls work on it
     * - Get the current mode - is it already in monitor mode?  If so, we're done
     *   and the world is good
     * - Check and warn about reg domain
     * - Check for rfkill
     * - It's not in monitor mode.  Try to make a VIF via mac80211 for it; this is
     *   by far the most likely to succeed on modern systems.
     * - Figure out if we can name the vif something sane under new interface
     *   naming rules; preferably interfaceXmon
     * - Extract channels
     * - Generate UUID
     * - Initiate pcap
     */

    local_wifi_t *local_wifi = (local_wifi_t *) caph->userdata;

    char *placeholder = NULL;
    int placeholder_len;
    
    uint8_t hwaddr[6];

    char errstr[STATUS_MAX];
    char errstr2[STATUS_MAX];
    char pcap_errstr[PCAP_ERRBUF_SIZE] = "";

    char ifnam[IFNAMSIZ];

    *uuid = NULL;
    *chanset = NULL;
    *chanlist = NULL;
    *chanlist_sz = 0;

    int mode;

    int ret;

#ifdef HAVE_LIBNM
    NMClient *nmclient = NULL;
    NMDevice *nmdevice = NULL;
    const GPtrArray *nmdevices;
    GError *nmerror = NULL;
#endif

    if ((placeholder_len = cf_parse_interface(&placeholder, definition)) <= 0) {
        snprintf(msg, STATUS_MAX, "Unable to find interface in definition"); 
        return -1;
    }

    local_wifi->interface = strndup(placeholder, placeholder_len);

    /* get the mac address; this should be standard for anything */
    if (ifconfig_get_hwaddr(local_wifi->interface, errstr, hwaddr) < 0) {
        snprintf(msg, STATUS_MAX, "Could not fetch interface address from '%s': %s",
                local_wifi->interface, errstr);
        return -1;
    }

    /* if we're hard rfkilled we can't do anything */
    if (linux_sys_get_rfkill(local_wifi->interface, LINUX_RFKILL_TYPE_HARD) == 1) {
        snprintf(msg, STATUS_MAX, "Interface '%s' is set to hard rfkill; check your "
                "wireless switch if you have one.", local_wifi->interface);
        return -1;
    }

    /* if we're soft rfkilled, unkill us */
    if (linux_sys_get_rfkill(local_wifi->interface, LINUX_RFKILL_TYPE_SOFT) == 1) {
        if (linux_sys_clear_rfkill(local_wifi->interface) < 0) {
            snprintf(msg, STATUS_MAX, "Unable to activate interface '%s' set to "
                    "soft rfkill", local_wifi->interface);
            return -1;
        }
        snprintf(errstr, STATUS_MAX, "Removed soft-rfkill and enabled interface '%s'", 
                local_wifi->interface);
        cf_send_message(caph, errstr, MSGFLAG_INFO);
    }

    /* Make a spoofed, but consistent, UUID based on the adler32 of the interface name 
     * and the mac address of the device */
    snprintf(errstr, STATUS_MAX, "%08X-0000-0000-0000-%02X%02X%02X%02X%02X%02X",
            adler32_csum((unsigned char *) "kismet_cap_linux_wifi", 
                strlen("kismet_cap_linux_wifi")) & 0xFFFFFFFF,
            hwaddr[0] & 0xFF, hwaddr[1] & 0xFF, hwaddr[2] & 0xFF,
            hwaddr[3] & 0xFF, hwaddr[4] & 0xFF, hwaddr[5] & 0xFF);
    *uuid = strdup(errstr);

    fprintf(stderr, "debug - generated uuid %s for %s\n", *uuid, local_wifi->interface);

    /* Try to get it into monitor mode if it isn't already; even mac80211 drivers
     * respond to SIOCGIWMODE */
    if (iwconfig_get_mode(local_wifi->interface, errstr, &mode) < 0) {
        snprintf(msg, STATUS_MAX, "Unable to get current wireless mode of "
                "interface '%s': %s", local_wifi->interface, errstr);
        return -1;
    }

    /* We think we can do something with this interface; if we have support,
     * connect to network manager.  Because it looks like nm keeps trying
     * to deliver reports to us as long as we're connected, DISCONNECT 
     * when we're done! */
#ifdef HAVE_LIBNM
    nmclient = nm_client_new(NULL, &nmerror);

    if (nmclient == NULL) {
        if (nmerror != NULL) {
            snprintf(errstr, STATUS_MAX, "Could not connect to NetworkManager, "
                    "cannot automatically prevent interface '%s' from being "
                    "modified if NetworkManager is running: %s",
                    local_wifi->interface, nmerror->message);
        } else {
            snprintf(errstr, STATUS_MAX, "Could not connect to NetworkManager, "
                    "cannot automatically prevent interface '%s' from being "
                    "modified if NetworkManager is running.",
                    local_wifi->interface);
        }

        cf_send_message(caph, errstr, MSGFLAG_INFO);
    } else if (nm_client_get_nm_running(nmclient)) {
        nmdevices = nm_client_get_devices(nmclient);

        if (nmdevices != NULL) {
            for (int i = 0; i < nmdevices->len; i++) {
                const NMDevice *d = g_ptr_array_index(nmdevices, i);

                if (strcmp(nm_device_get_iface((NMDevice *) d), 
                            local_wifi->interface) == 0) {
                    nmdevice = (NMDevice *) d;
                    break;
                }
            }
        }
    }

    if (nmdevice != NULL) {
        local_wifi->reset_nm_management = nm_device_get_managed(nmdevice);

        if (local_wifi->reset_nm_management) {
            snprintf(errstr, STATUS_MAX, "Telling NetworkManager not to control "
                    "interface '%s': you may need to re-initialize this interface "
                    "later or tell NetworkManager to control it again via 'nmcli'",
                    local_wifi->interface);
            cf_send_message(caph, errstr, MSGFLAG_INFO);
            nm_device_set_managed(nmdevice, 0);
        }
    }

    /* We HAVE to unref the nmclient and disconnect here or it keeps trying
     * to deliver messages to us, filling up hundreds of megs of ram */
    if (nmclient != NULL)
        g_object_unref(nmclient);

#endif

    if (mode != LINUX_WLEXT_MONITOR) {
        int existing_ifnum;

        /* Look to see if there's a vif= flag specified on the source line; this
         * takes precedence over everything */
        if ((placeholder_len = cf_find_flag(&placeholder, "vif", definition)) > 0) {
            local_wifi->cap_interface = strndup(placeholder, placeholder_len);
        } else {
            /* Look for an interface that shares the mac and is in monitor mode */
            existing_ifnum = 
                find_interface_mode_by_mac(local_wifi->interface, 
                        LINUX_WLEXT_MONITOR, hwaddr);

            if (existing_ifnum >= 0) {
                if (if_indextoname((unsigned int) existing_ifnum, ifnam) != NULL) {
                    local_wifi->cap_interface = strdup(ifnam);
                }
            }
        }
            
        /* Otherwise try to come up with a monitor name */
        if (local_wifi->cap_interface == NULL) {
            /* First we'd like to make a monitor vif if we can; can we fit that
             * in our interface name?  */
            if (strlen(local_wifi->interface) + 3 >= IFNAMSIZ) {
                /* Can't fit our name in, we have to make an unrelated name, 
                 * we'll call it 'kismonX'; find the next kismonX interface */
                int ifnum = find_next_ifnum("kismon");

                if (ifnum < 0) {
                    snprintf(msg, STATUS_MAX, "Could not append 'mon' extension to "
                            "existing interface (%s) and could not find a kismonX "
                            "within 100 tries", local_wifi->interface);
                    return -1;
                }

                /* We know we're ok here; we got this by figuring out nothing
                 * matched and then enumerating our own */
                snprintf(ifnam, IFNAMSIZ, "kismon%d", ifnum);
            } else {
                snprintf(ifnam, IFNAMSIZ, "%smon", local_wifi->interface);

                /* We need to check the mode here to make sure we're not in a weird
                 * state where NM retyped our interface or something */
                if (iwconfig_get_mode(ifnam, errstr, &mode) >= 0) {
                    if (mode != LINUX_WLEXT_MONITOR) {
                        snprintf(msg, STATUS_MAX, "A monitor vif already exists "
                                "for interface '%s' (%s) but isn't in monitor mode, "
                                "check that NetworkManager isn't hijacking the "
                                "interface, delete the false monitor vif, and try "
                                "again.", local_wifi->interface, ifnam);
                        return -1;
                    }
                }
            }

            /* Dup our monitor interface name */
            local_wifi->cap_interface = strdup(ifnam);
        }

    } else {
        /* Otherwise the capinterface equals the interface because it's 
         * already in monitor mode */
        local_wifi->cap_interface = strdup(local_wifi->interface);
    }

    /* We think we know what we're going to capture from now; see if it already 
     * exists and is in monitor mode; we may be doing multiple mode fetches
     * but it doesn't really matter much; it's a simple ioctl and it only
     * happens during open; we tolerate a failure here since the interface
     * might not exist! */

    if (iwconfig_get_mode(local_wifi->interface, errstr, &mode) < 0) 
        mode = -1;

    /* We're going to start interacting with devices - connect to mac80211 if
     * we can; an error here is tolerable because we'll fail properly later
     * on */
    local_wifi->mac80211_handle = NULL;
    local_wifi->mac80211_cache = NULL;
    local_wifi->mac80211_family = NULL;

    if (mac80211_connect(local_wifi->interface, &(local_wifi->mac80211_handle),
                &(local_wifi->mac80211_cache), &(local_wifi->mac80211_family),
                errstr) < 0) {
        local_wifi->mac80211_handle = NULL;
        local_wifi->mac80211_cache = NULL;
        local_wifi->mac80211_family = NULL;
    }

    /* The interface we want to use isn't in monitor mode - and presumably
     * doesn't exist - so try to make a monitor vif via mac80211; this will 
     * work with all modern drivers and we'd definitely rather do this.
     */
    if (mode != LINUX_WLEXT_MONITOR && local_wifi->mac80211_handle != NULL &&
            strcmp(local_wifi->interface, local_wifi->cap_interface) != 0) {
        /* First, look for some nl80211 flags in the arguments. */
        unsigned int num_flags = 2;
        unsigned int fi;
        unsigned int *flags = NULL;

        bool fcs = false;
        bool plcp = false;

        if ((placeholder_len = cf_find_flag(&placeholder, "fcsfail", definition)) > 0) {
            if (strncasecmp(placeholder, "true", placeholder_len) == 0) {
                snprintf(errstr, STATUS_MAX,
                        "Source '%s' configuring monitor interface to pass packets "
                        "which fail FCS checksum", local_wifi->interface);
                cf_send_message(caph, errstr, MSGFLAG_INFO);
                num_flags++;
                fcs = true;
            }
        }

        if ((placeholder_len = cf_find_flag(&placeholder, "plcpfail", 
                        definition)) > 0) {
            if (strncasecmp(placeholder, "true", placeholder_len) == 0) {
                snprintf(errstr, STATUS_MAX,
                        "Source '%s' configuring monitor interface to pass packets "
                        "which fail PLCP checksum", local_wifi->interface);
                cf_send_message(caph, errstr, MSGFLAG_INFO);
                num_flags++;
                plcp = true;
            }
        }

        /* Allocate the flag list */
        flags = (unsigned int *) malloc(sizeof(unsigned int) * num_flags);

        /* We always set these */
        fi = 0;
        flags[fi++] = NL80211_MNTR_FLAG_CONTROL;
        flags[fi++] = NL80211_MNTR_FLAG_OTHER_BSS;

        if (fcs)
            flags[fi++] = NL80211_MNTR_FLAG_FCSFAIL;

        if (plcp)
            flags[fi++] = NL80211_MNTR_FLAG_PLCPFAIL;

        /* Try to make the monitor vif */
        if (mac80211_create_monitor_vif(local_wifi->interface,
                    local_wifi->cap_interface, flags, num_flags, errstr) < 0) {
            /* Send an error message */
            snprintf(errstr2, STATUS_MAX, "Failed to create monitor vif interface '%s' "
                    "for interface '%s': %s", local_wifi->cap_interface,
                    local_wifi->interface, errstr);
            cf_send_message(caph, errstr2, MSGFLAG_ERROR);

            /* Try to switch the mode of this interface to monitor; maybe we're a
             * wlext device after all */
            if (iwconfig_set_mode(local_wifi->interface, errstr, 
                        LINUX_WLEXT_MONITOR) < 0) {
                snprintf(errstr2, STATUS_MAX, "Failed to put interface '%s' in monitor "
                        "mode: %s", local_wifi->interface, errstr);
                cf_send_message(caph, errstr2, MSGFLAG_ERROR);

                /* We've failed at everything */
                snprintf(msg, STATUS_MAX, "Failed to create a monitor vif and could "
                        "not set mode of existing interface, unable to put "
                        "'%s' into monitor mode.", local_wifi->interface);

                free(flags);

                return -1;
            } else {
                snprintf(errstr2, STATUS_MAX, "Configured '%s' as monitor mode "
                        "interface instead of using a monitor vif; will continue using "
                        "this interface as the capture source.", local_wifi->interface);
                cf_send_message(caph, errstr2, MSGFLAG_INFO);

                mac80211_disconnect(local_wifi->mac80211_handle, 
                        local_wifi->mac80211_cache);
                local_wifi->mac80211_cache = NULL;
                local_wifi->mac80211_family = NULL;
                local_wifi->mac80211_handle = NULL;
            }
        } else {
            snprintf(errstr2, STATUS_MAX, "Successfully created monitor interface "
                    "'%s' for interface '%s'", local_wifi->cap_interface,
                    local_wifi->interface);
            local_wifi->use_mac80211 = 1;
        }

        free(flags);
    } else if (mode != LINUX_WLEXT_MONITOR) {
        /* Otherwise we want monitor mode but we don't have nl / found the same vif */
        if (iwconfig_set_mode(local_wifi->interface, errstr, 
                    LINUX_WLEXT_MONITOR) < 0) {
            snprintf(errstr2, STATUS_MAX, "Failed to put interface '%s' in monitor "
                    "mode: %s", local_wifi->interface, errstr);
            cf_send_message(caph, errstr2, MSGFLAG_ERROR);

            /* We've failed at everything */
            snprintf(msg, STATUS_MAX, "Could not create a monitor vif and could "
                    "not set mode of existing interface, unable to put "
                    "'%s' into monitor mode.", local_wifi->interface);
            return -1;
        } else {
            snprintf(errstr2, STATUS_MAX, "Configured '%s' as monitor mode "
                    "interface instead of using a monitor vif",
                    local_wifi->interface);
            cf_send_message(caph, errstr2, MSGFLAG_INFO);
        }
    } else {
        snprintf(errstr, STATUS_MAX, "Monitor interface '%s' already exists for "
                "capture interface '%s', we'll use that.",
                local_wifi->interface, local_wifi->cap_interface);
        cf_send_message(caph, errstr, MSGFLAG_INFO);
    }

    /* If we're using a vif we need to bring down the parent and bring up the vif;
     * if we're not using a vif we just need to bring up the interface */
    if (strcmp(local_wifi->interface, local_wifi->cap_interface) != 0) {
        int ign_primary = 0;
        if ((placeholder_len = cf_find_flag(&placeholder, "ignoreprimary", 
                        definition)) > 0) {
            if (strncasecmp(placeholder, "true", placeholder_len) == 0) {
                snprintf(errstr, STATUS_MAX,
                        "Source '%s' ignoring state of primary interface and "
                        "leaving it in an 'up' state; this may cause problems "
                        "with channel hopping.", local_wifi->interface);
                cf_send_message(caph, errstr, MSGFLAG_INFO);
                ign_primary = 1;
            }
        }

        if (!ign_primary) {
            snprintf(errstr2, STATUS_MAX, "Bringing down parent interface '%s'",
                    local_wifi->interface);
            cf_send_message(caph, errstr2, MSGFLAG_INFO);

            if (ifconfig_interface_down(local_wifi->interface, errstr) != 0) {
                snprintf(msg, STATUS_MAX, "Could not bring down parent interface "
                        "'%s' to capture using '%s': %s", local_wifi->interface,
                        local_wifi->cap_interface, errstr);
                return -1;
            }
        }
    }

    /* Bring up the cap interface no matter what */
    if (ifconfig_interface_up(local_wifi->cap_interface, errstr) != 0) {
        snprintf(msg, STATUS_MAX, "Could not bring up capture interface '%s', "
                "check 'dmesg' for errors loading firmware: %s",
                local_wifi->cap_interface, errstr);
        return -1;
    }

    ret = populate_chanlist(local_wifi->cap_interface, errstr, chanlist, chanlist_sz);
    if (ret < 0) {
        snprintf(msg, STATUS_MAX, "Could not get list of channels from capture "
                "interface '%s' on '%s': %s", local_wifi->cap_interface,
                local_wifi->interface, errstr);
        return -1;
    }

    /* Open the pcap */
    local_wifi->pd = pcap_open_live(local_wifi->cap_interface, 
            MAX_PACKET_LEN, 1, 1000, pcap_errstr);

    if (local_wifi->pd == NULL || strlen(pcap_errstr) != 0) {
        snprintf(msg, STATUS_MAX, "Could not open capture interface '%s' on '%s' "
                "as a pcap capture: %s", local_wifi->cap_interface, 
                local_wifi->interface, pcap_errstr);
        return -1;
    }

    local_wifi->datalink_type = pcap_datalink(local_wifi->pd);

    if (strcmp(local_wifi->interface, local_wifi->cap_interface) != 0) {
        snprintf(msg, STATUS_MAX, "Linux Wi-Fi capturing from monitor vif '%s' on "
                "interface '%s'", local_wifi->cap_interface, local_wifi->interface);
    } else {
        snprintf(msg, STATUS_MAX, "Linux Wi-Fi capturing from interface '%s'",
                local_wifi->interface);
    }

    return 1;
}

int list_callback(kis_capture_handler_t *caph, uint32_t seqno,
        char *msg, char ***interfaces, char ***flags) {
    DIR *devdir;
    struct dirent *devfile;
    char errstr[STATUS_MAX];

    /* Basic list of devices */
    typedef struct wifi_list {
        char *device;
        char *flags;
        struct wifi_list *next;
    } wifi_list_t; 

    wifi_list_t *devs = NULL;
    size_t num_devs = 0;

    unsigned int i;

    if ((devdir = opendir("/sys/class/net/")) == NULL) {
        fprintf(stderr, "debug - no /sys/class/net dir?\n");

        /* Not an error, just nothing to do */
        *interfaces = NULL;
        *flags = NULL;
        return 0;
    }

    /* Look at the files in the sys dir and see if they're wi-fi */
    while ((devfile = readdir(devdir)) != NULL) {
        /* if we can get the current channel with simple iwconfig ioctls
         * it's definitely a wifi device; even mac80211 devices respond 
         * to it */
        if (iwconfig_get_channel(devfile->d_name, errstr) > 0) {
            wifi_list_t *d = (wifi_list_t *) sizeof(wifi_list_t);
            fprintf(stderr, "debug - found wireless device %s\n", devfile->d_name);
            num_devs++;
            d->device = strdup(devfile->d_name);
            d->flags = NULL;
            d->next = devs;
            devs = d;
        }
    }

    closedir(devdir);

    if (num_devs == 0) {
        *interfaces = NULL;
        *flags = NULL;
        return 0;
    }

    *interfaces = (char **) malloc(sizeof(char *) * num_devs);
    *flags = (char **) malloc(sizeof(char *) * num_devs);

    i = 0;
    while (devs != NULL) {
        wifi_list_t *td = devs->next;
        (*interfaces)[i] = devs->device;
        (*flags)[i] = devs->flags;

        free(devs);
        devs = td;

        i++;
    }

    return num_devs;
}

/* Channel control callback; actually set a channel.  Determines if our
 * custom channel needs a VHT frequency set. */
int chancontrol_callback(kis_capture_handler_t *caph, uint32_t seqno, void *privchan,
        char *msg) {
    local_wifi_t *local_wifi = (local_wifi_t *) caph->userdata;
    local_channel_t *channel = (local_channel_t *) privchan;
    int r;
    char errstr[STATUS_MAX];
    char chanstr[STATUS_MAX];

    if (privchan == NULL)
        return 0;

    if (local_wifi->use_mac80211 == 0) {
        if ((r = iwconfig_set_channel(local_wifi->interface, 
                        channel->control_freq, errstr)) < 0) {
            /* Sometimes tuning a channel fails; this is only a problem if we fail
             * to tune a channel a bunch of times.  Spit out a tuning error at first;
             * if we continually fail, if we have a seqno we're part of a CONFIGURE
             * command and we send a configresp, otherwise send an error 
             *
             * If seqno == 0 we're inside the chanhop, so we can tolerate failures.
             * If we're sending an explicit channel change command, error out
             * immediately.
             *
             * */
            if (local_wifi->seq_channel_failure < 10 && seqno == 0) {
                local_channel_to_str(channel, chanstr);
                snprintf(msg, STATUS_MAX, "Could not set channel %s; ignoring error "
                        "and continuing (%s)", chanstr, errstr);
                cf_send_message(caph, msg, MSGFLAG_ERROR);
                return 0;
            } else {
                local_channel_to_str(channel, chanstr);
                snprintf(msg, STATUS_MAX, "failed to set channel %s: %s", 
                        chanstr, errstr);

                if (seqno == 0) {
                    cf_send_error(caph, msg);
                }

                return -1;
            }
        } else {
            local_wifi->seq_channel_failure = 0;

            if (seqno != 0) {
                /* Send a config response with a reconstituted channel if we're
                 * configuring the interface; re-use errstr as a buffer */
                local_channel_to_str(channel, errstr);
                cf_send_configresp_channel(caph, seqno, 1, NULL, errstr);
            }
        }

        return 0;
    } else {
        /* Otherwise we're using mac80211 which means we need to figure out
         * what kind of channel we're setting */
        if (channel->chan_width != 0) {
            /* An explicit channel width means we need to use _set_freq to set
             * a control freq, a width, and possibly an extended center frequency
             * for VHT; if center1 is 0 _set_frequency will automatically
             * exclude it and only set the width */
            r = mac80211_set_frequency_cache(local_wifi->cap_interface,
                    local_wifi->mac80211_handle, local_wifi->mac80211_family,
                    channel->control_freq, channel->chan_width,
                    channel->center_freq1, channel->center_freq2, errstr);
        } else {
            /* Otherwise for HT40 and non-HT channels, set the channel w/ any
             * flags present */
            r = mac80211_set_channel_cache(local_wifi->cap_interface,
                    local_wifi->mac80211_handle, local_wifi->mac80211_family,
                    channel->control_freq, channel->chan_type, errstr);
        } 

        /* Handle channel set results */
        if (r < 0) {
            /* If seqno == 0 we're inside the chanhop, so we can tolerate failures.
             * If we're sending an explicit channel change command, error out
             * immediately.
             */
            if (local_wifi->seq_channel_failure < 10 && seqno == 0) {
                local_channel_to_str(channel, chanstr);
                snprintf(msg, STATUS_MAX, "Could not set channel %s; ignoring error "
                        "and continuing (%s)", chanstr, errstr);
                cf_send_message(caph, msg, MSGFLAG_ERROR);
                return 0;
            } else {
                local_channel_to_str(channel, chanstr);
                snprintf(msg, STATUS_MAX, "failed to set channel %s: %s", 
                        chanstr, errstr);

                if (seqno == 0) {
                    cf_send_error(caph, msg);
                }

                return -1;
            }
        } else {
            local_wifi->seq_channel_failure = 0;
            return 0;
        }
    }
   
    return 0;
}

void pcap_dispatch_cb(u_char *user, const struct pcap_pkthdr *header,
        const u_char *data)  {
    kis_capture_handler_t *caph = (kis_capture_handler_t *) user;
    local_wifi_t *local_wifi = (local_wifi_t *) caph->userdata;
    int ret;

    // fprintf(stderr, "debug - pcap_dispatch - got packet %u\n", header->caplen);

    /* Try repeatedly to send the packet; go into a thread wait state if
     * the write buffer is full & we'll be woken up as soon as it flushes
     * data out in the main select() loop */
    while (1) {
        if ((ret = cf_send_data(caph, 
                        NULL, NULL, NULL,
                        header->ts, local_wifi->datalink_type,
                        header->caplen, (uint8_t *) data)) < 0) {
            fprintf(stderr, "debug - linux_wifi - cf_send_data failed\n");
            pcap_breakloop(local_wifi->pd);
            cf_send_error(caph, "unable to send DATA frame");
            cf_handler_spindown(caph);
        } else if (ret == 0) {
            /* Go into a wait for the write buffer to get flushed */
            cf_handler_wait_ringbuffer(caph);
            continue;
        } else {
            break;
        }
    }
}

void capture_thread(kis_capture_handler_t *caph) {
    local_wifi_t *local_wifi = (local_wifi_t *) caph->userdata;
    char errstr[PCAP_ERRBUF_SIZE];
    char *pcap_errstr;
    char iferrstr[STATUS_MAX];
    int ifflags = 0, ifret;

    /* Simple capture thread: since we don't care about blocking and 
     * channel control is managed by the channel hopping thread, all we have
     * to do is enter a blocking pcap loop */

    fprintf(stderr, "debug - pcap_loop\n");

    pcap_loop(local_wifi->pd, -1, pcap_dispatch_cb, (u_char *) caph);

    pcap_errstr = pcap_geterr(local_wifi->pd);

    snprintf(errstr, PCAP_ERRBUF_SIZE, "Interface '%s' closed: %s", 
            local_wifi->cap_interface, 
            strlen(pcap_errstr) == 0 ? "interface closed" : pcap_errstr );

    fprintf(stderr, "debug - %s\n", errstr);

    cf_send_error(caph, errstr);

    ifret = ifconfig_get_flags(local_wifi->cap_interface, iferrstr, &ifflags);

    if (ifret < 0 || !(ifflags & IFF_UP)) {
        snprintf(errstr, PCAP_ERRBUF_SIZE, "Interface '%s' no longer appears to be up; "
                "This can happen when it is unplugged, or another service like DHCP or "
                "NetworKManager has taken over and shut it down on us.", 
                local_wifi->cap_interface);
        cf_send_error(caph, errstr);
    }

    cf_handler_spindown(caph);

    fprintf(stderr, "debug - linux wifi - capture thread finishing\n");
}

int main(int argc, char *argv[]) {
    local_wifi_t local_wifi = {
        .pd = NULL,
        .interface = NULL,
        .cap_interface = NULL,
        .datalink_type = -1,
        .override_dlt = -1,
        .use_mac80211 = 1,
        .mac80211_cache = NULL,
        .mac80211_handle = NULL,
        .mac80211_family = NULL,
        .seq_channel_failure = 0,
        .reset_nm_management = 0,
    };

#ifdef HAVE_LIBNM
    NMClient *nmclient = NULL;
    const GPtrArray *nmdevices;
    GError *nmerror = NULL;
#endif

#if 0
    /* Remap stderr so we can log debugging to a file */
    FILE *sterr;
    sterr = fopen("/tmp/capture_linux_wifi.stderr", "a");
    dup2(fileno(sterr), STDERR_FILENO);
    dup2(fileno(sterr), STDOUT_FILENO);
#endif

    fprintf(stderr, "CAPTURE_LINUX_WIFI launched on pid %d\n", getpid());

    kis_capture_handler_t *caph = cf_handler_init();

    if (caph == NULL) {
        fprintf(stderr, "FATAL: Could not allocate basic handler data, your system "
                "is very low on RAM or something is wrong.\n");
        return -1;
    }

    if (cf_handler_parse_opts(caph, argc, argv) < 1) {
        fprintf(stderr, "FATAL: Missing command line parameters.\n");
        return -1;
    }

    /* Set the local data ptr */
    cf_handler_set_userdata(caph, &local_wifi);

    /* Set the callback for opening  */
    cf_handler_set_open_cb(caph, open_callback);

    /* Set the callback for probing an interface */
    cf_handler_set_probe_cb(caph, probe_callback);

    /* Set the translation cb */
    cf_handler_set_chantranslate_cb(caph, chantranslate_callback);

    /* Set the control cb */
    cf_handler_set_chancontrol_cb(caph, chancontrol_callback);

    /* Set the capture thread */
    cf_handler_set_capture_cb(caph, capture_thread);

    /* Set a channel hop spacing of 4 to get the most out of 2.4 overlap;
     * it does nothing and hurts nothing on 5ghz */
    cf_handler_set_hop_shuffle_spacing(caph, 4);

    cf_handler_loop(caph);

    /* We're done - try to reset the networkmanager awareness of the interface */

#ifdef HAVE_LIBNM
    if (local_wifi.reset_nm_management) {
        nmclient = nm_client_new(NULL, &nmerror);

        if (nmclient != NULL) {
            if (nm_client_get_nm_running(nmclient)) {
                nmdevices = nm_client_get_devices(nmclient);

                if (nmdevices != NULL) {
                    for (int i = 0; i < nmdevices->len; i++) {
                        const NMDevice *d = g_ptr_array_index(nmdevices, i);

                        if (strcmp(nm_device_get_iface((NMDevice *) d), 
                                    local_wifi.interface) == 0) {
                            nm_device_set_managed((NMDevice *) d, 1);
                            break;
                        }
                    }
                }
            }

            g_object_unref(nmclient);
        }
    }
#endif

    fprintf(stderr, "FATAL: Exited main select() loop, waiting to be killed\n");

    cf_handler_free(caph);

    while (1) {
        sleep(1);
    }

    return 1;
}
