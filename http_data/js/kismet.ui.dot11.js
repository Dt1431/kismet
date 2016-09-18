(
  typeof define === "function" ? function (m) { define("kismet-ui-dot11-js", m); } :
  typeof exports === "object" ? function (m) { module.exports = m(); } :
  function(m){ this.kismet_ui_dot11 = m(); }
)(function () {

"use strict";

var exports = {};

// Flag we're still loading
exports.load_complete = 0;

/* Define some callback functions for the table */

console.log("adding device detail 'dot11'");
kismet_ui.AddDeviceDetail("dot11", "Wi-Fi (802.11)", 0, {
    draw: function(data, target) {
        target.devicedata(data, {
            "id": "dot11DeviceData",
            "fields": [
            {
                field: "dot11_device.dot11_device_last_beaconed_ssid",
                title: "Last Beaconed SSID (AP)",
                empty: "<i>None</i>"
            },
            {
                field: "dot11_device.dot11_device_last_probed_ssid",
                title: "Last Probed SSID (Client)",
                empty: "<i>None</i>"
            },
            {
                field: "dot11_device.dot11_device_last_bssid",
                title: "Last BSSID",
                filter: function(key, data, value) {
                    return value.split('/')[0] !== '00:00:00:00:00:00';
                },
                render: function(key, data, value) {
                    return value.split('/')[0];
                }
            },
            {
                field: "dot11_packet_group",
                groupTitle: "Packets",
                id: "dot11_packet_group",

                fields: [
                {
                    field: "graph_field_dot11",
                    span: true,
                    render: function(key, data, value) {
                        return '<div class="smalldonut" id="overall" style="float: left;" />' +
                            '<div class="smalldonut" id="data" style="float: right;" />';
                    },
                    draw: function(key, data, value, container) {
                        var overalldiv = $('#overall', container);
                        var datadiv = $('#data', container);

                        // Make an array morris likes using our whole data record
                        var modoverall = [
                        { label: "Mgmt", value: data.kismet_device_base_packets_llc },
                        { label: "Data", value: data.kismet_device_base_packets_data }
                        ];

                        if (data.kismet_device_base_packets_error != 0)
                            modoverall.push({ label: "Error", value: data.kismet_device_base_packets_error });

                        Morris.Donut({
                            element: overalldiv,
                            data: modoverall
                        });

                        var moddata = [
                        { label: "Data", value: data.kismet_device_base_packets_data },
                        { label: "Retry", value: data.dot11_device.dot11_device_num_retries },
                        { label: "Frag", value: data.dot11_device.dot11_device_num_fragments }
                        ];

                        Morris.Donut({
                            element: datadiv,
                            data: moddata
                        });
                    }
                },
                {
                    field: "kismet_device_base_packets_total",
                    title: "Total Packets"
                },

                {
                    field: "dot11_device.dot11_device_num_fragments",
                    title: "Fragmented Packets"
                },
                {
                    field: "dot11_device.dot11_device_num_retries",
                    title: "Retried Packets"
                },
                {
                    field: "dot11_device.dot11_device_datasize",
                    title: "Data",
                    render: function(key, data, value) {
                        return kismet.HumanReadableSize(value);
                    }
                },
                {
                    field: "dot11_device.dot11_device_datasize_retry",
                    title: "Retried Data",
                    render: function(key, data, value) {
                        return kismet.HumanReadableSize(value);
                    }
                }
                ],
            },
            ]
        });
    }
});

console.log("kismet.ui.dot11.js returning, we think we loaded everything?");

// We're done loading
exports.load_complete = 1;

return exports;

});

