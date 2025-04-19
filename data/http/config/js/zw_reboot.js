var DEVMODE = window.location.protocol === 'file:';
const URL_REBOOT = "/!sys/reboot?";

function redirect() {
  window.location.href = document.referrer;
}

function reboot(boot_serial) {
  $.get({
    url: URL_REBOOT + $.param({ "bs": boot_serial }),
  }).done(function () {
    console.log("Reboot in progress...");
    setTimeout(function () { probe_boot_serial_for(redirect, null); }, STATUS_PROBE_INTERVAL);
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    console.log("Reboot failed" + (resp_text ? ": " + resp_text : "."));
    redirect();
  });
}

$(function () {
  if (!DEVMODE) {
    probe_boot_serial_for(reboot, null);
  }
});