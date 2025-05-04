var DEVMODE = window.location.protocol === 'file:';
const URL_REBOOT = "/!sys/reboot?";

function navigate_back() {
  history.back();
}

function request_reboot(boot_serial) {
  $("#page-content").text(`Requesting reboot...`);
  probe_url_for(URL_REBOOT + $.param({ "bs": boot_serial }), function () {
    $("#page-content").text(`Reboot in progress...`);
    setTimeout(function () {
      probe_url_for(URL_BOOT_SERIAL, navigate_back, function (text) {
        $("#page-content").text(`Failed to probe boot serial: ${text}`);
        setTimeout(function () { navigate_back(); }, STATUS_PROBE_INTERVAL);
      });
    }, STATUS_PROBE_INTERVAL);
  }, function (text) {
    $("#page-content").text(`Reboot request failed: ${text}`);
    setTimeout(function () { navigate_back(); }, STATUS_PROBE_INTERVAL);
  });
}

$(function () {
  if (!DEVMODE) {
    probe_url_for(URL_BOOT_SERIAL, request_reboot, function (text) {
      $("#page-content").text(`Failed to probe boot serial: ${text}`);
      setTimeout(function () { navigate_back(); }, STATUS_PROBE_INTERVAL);
    });
  } else {
    setTimeout(function () { navigate_back(); }, STATUS_PROBE_INTERVAL);
  }
});