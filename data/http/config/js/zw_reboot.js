var DEVMODE = window.location.protocol === 'file:';
const URL_REBOOT = "/!sys/reboot?";

function redirect() {
  window.location.href = document.referrer;
}

function request_reboot(boot_serial) {
  $("#page-content").text(`Requesting reboot...`);
  probe_url_for(URL_REBOOT + $.param({ "bs": boot_serial }), function () {
    $("#page-content").text(`Reboot in progress...`);
    setTimeout(function () {
      probe_url_for(URL_BOOT_SERIAL, redirect, function (text) {
        $("#page-content").text(`Failed to probe boot serial: ${text}`);
        setTimeout(function () { redirect(); }, STATUS_PROBE_INTERVAL);
      }, null);
    }, STATUS_PROBE_INTERVAL);
  }, function (text) {
    $("#page-content").text(`Reboot request failed: ${text}`);
    setTimeout(function () { redirect(); }, STATUS_PROBE_INTERVAL);
  }, null);
}

$(function () {
  if (!DEVMODE) {
    probe_url_for(URL_BOOT_SERIAL, request_reboot, function (text) {
      $("#page-content").text(`Failed to probe boot serial: ${text}`);
      setTimeout(function () { redirect(); }, STATUS_PROBE_INTERVAL);
    }, null);
  }
});