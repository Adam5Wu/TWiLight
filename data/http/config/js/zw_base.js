const URL_BOOT_SERIAL = "/!sys/boot_serial";

//----------------------------------------
// System Management utility functions
const STATUS_PROBE_INTERVAL = 3000;

function probe_url_for(url, success_action, fail_action, data) {
  $.when($.get(url)).then(function (payload) {
    console.log(`Probe '${url}' succeeded:`, payload);
    success_action(payload, data);
  }, function (jqXHR, textStatus) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    console.log(`Probe '${url}' failed: ${resp_text || textStatus}`);
    // Keep retrying if no response text (likely timed out)
    if (!resp_text) {
      setTimeout(function () {
        probe_url_for(url, success_action, fail_action, data);
      }, STATUS_PROBE_INTERVAL);
    } else fail_action(resp_text, data);
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

function dialog_close(trigger_action = false) {
  var dialog = $("#dialog");
  if (!trigger_action) dialog.off();
  dialog.data("action", "close");
  dialog.close();
}

function block_prompt(message) {
  var dialog = $("#dialog");
  var dialog_msg = $("#dlg-message");
  var dialog_ok = $("#dlg-submit");
  var dialog_cancel = $("#dlg-reset");
  dialog_msg.html("<p>" + message);
  dialog_ok.hide();
  dialog_cancel.hide();
  dialog.showModal();
  dialog.off();
  dialog.on("keydown", function (evt) { evt.preventDefault(); });
}

function notify_prompt(message, action) {
  var dialog = $("#dialog");
  var dialog_msg = $("#dlg-message");
  var dialog_ok = $("#dlg-submit");
  var dialog_cancel = $("#dlg-reset");
  dialog_msg.html("<p>" + message);
  dialog_ok.show();
  dialog_cancel.hide();
  dialog.showModal();
  dialog.off();
  if (typeof action === 'function') {
    dialog.on("close", function () { dialog.off(); action(); });
  }
}

function op_confirm_prompt(message, action, data) {
  var dialog = $("#dialog");
  var dialog_msg = $("#dlg-message");
  var dialog_ok = $("#dlg-submit");
  var dialog_cancel = $("#dlg-reset");
  dialog_msg.html("<p>" + message);
  dialog_ok.show();
  dialog_cancel.show();
  dialog.showModal();
  dialog.off();
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