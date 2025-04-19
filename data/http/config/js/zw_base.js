//----------------------------------------
// System Management utility functions
const STATUS_PROBE_INTERVAL = 3000;
const URL_BOOT_SERIAL = "/!sys/boot_serial";

function probe_boot_serial_for(action, data) {
  $.when($.get(URL_BOOT_SERIAL)).then(function (text) {
    console.log("Received boot serial:", text);
    action(text, data);
  }, function (jqXHR, textStatus) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    console.log("Unable to obtain boot serial:", resp_text || textStatus);
    // Keep retrying if no response text (likely timed out)
    if (!resp_text) setTimeout(function () { probe_boot_serial_for(action, data); }, STATUS_PROBE_INTERVAL);
  });
}

//------------------------------
// Dialog utility functions

function dialog_submit() {
  var dialog = $("#dialog");
  dialog.data("action", "submit");
}

function dialog_cancel() {
  var dialog = $("#dialog");
  dialog.data("action", "cancel");
  dialog.close();
}

function notify_prompt(message) {
  var dialog = $("#dialog");
  var dialog_msg = $("#dlg-message");
  var dialog_cancel = $("#dlg-reset");
  dialog_msg.html("<p>" + message);
  dialog_cancel.hide();
  dialog.showModal();
}

function op_confirm_prompt(message, action, data) {
  var dialog = $("#dialog");
  var dialog_msg = $("#dlg-message");
  var dialog_cancel = $("#dlg-reset");
  dialog_msg.html("<p>" + message);
  dialog_cancel.show();
  dialog.showModal();
  dialog.on("close", function () { op_confirm_action(action, data); });
}

function op_confirm_action(action, data) {
  var dialog = $("#dialog");
  dialog.off();

  if (dialog.data("action") == "submit") {
    action(data);
  } else if (dialog.data("action") == "cancel") {
    console.log("Action cancelled by user.");
  } else {
    console.warn("Unexpected dialog close action: " + dialog.data("action"));
  }
}

$(function () {
  jQuery.fn.extend({
    showModal: function () {
      return this.each(function () {
        if (this.tagName === "DIALOG") {
          this.showModal();
        }
      });
    },
    close: function () {
      return this.each(function () {
        if (this.tagName === "DIALOG") {
          this.close();
        }
      });
    }
  });

  $("#dlg-submit").click(dialog_submit);
  $("#dlg-reset").click(dialog_cancel);

});