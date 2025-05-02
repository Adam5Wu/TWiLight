const ZWBASE_VERSION = "1.0.3"

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
var DIALOG;

function dialog_prompt_close(dialog, trigger_action = false) {
  if (!trigger_action) dialog.off();
  dialog.data("action", "close");
  dialog.close();
}

function prompt_close(trigger_action = false) {
  return dialog_prompt_close(DIALOG, trigger_action);
}

function dialog_op_setup(dialog, options) {
  // Set up controls
  var dialog_ok = dialog.find("input[type=submit]");
  var dialog_cancel = dialog.find("input[type=reset]");

  if (options['has_ok']) {
    dialog_ok.show();
    dialog_ok.attr('value', options['ok_text'] || "OK");
  } else {
    dialog_ok.hide();
  }
  if (options['has_cancel']) {
    dialog_cancel.show();
    dialog_cancel.attr('value', options['cancel_text'] || "Cancel");
  } else {
    dialog_cancel.hide();
  }

  // Clear extra controls from last use
  var extra_ctrl = dialog.data('extra-ctrl');
  if (extra_ctrl) {
    extra_ctrl.remove();
    dialog.data('extra-ctrl', null);
  }

  // Add extra controls for this use
  if ('extra-ctrl' in options) {
    extra_ctrl = options['extra-ctrl'];
    dialog_ok.before(extra_ctrl);
    dialog.data('extra-ctrl', extra_ctrl);
  }

  // Clear left-over event handlers from last use.
  dialog.off();
}

function dialog_block_prompt(dialog, message, extra_opt) {
  const options = $.extend({
    has_ok: false, has_cancel: false
  }, extra_opt);
  dialog_op_setup(dialog, options);
  var dialog_msg = dialog.find(".dlg-message");
  dialog_msg.html(message);
  dialog.showModal();
  dialog.on("keydown", function (evt) {
    if (evt.which == 27) evt.preventDefault();
  });
}

function block_prompt(message, extra_opt) {
  return dialog_block_prompt(DIALOG, message, extra_opt);
}

function dialog_notify_prompt(dialog, message, action, extra_opt) {
  const options = $.extend({
    has_ok: true, has_cancel: false
  }, extra_opt);
  dialog_op_setup(dialog, options);
  var dialog_msg = dialog.find(".dlg-message");
  dialog_msg.html(message);
  dialog.showModal();
  if (typeof action == 'function') {
    dialog.on("close", function () {
      dialog.off(); action(options['data']);
    });
  }
}

function notify_prompt(message, action, extra_opt) {
  return dialog_notify_prompt(DIALOG, message, action, extra_opt);
}

function dialog_confirm_prompt(dialog, message, action, extra_opt) {
  const options = $.extend({
    has_ok: true, has_cancel: true
  }, extra_opt);
  dialog_op_setup(dialog, options);
  var dialog_msg = dialog.find(".dlg-message");
  dialog_msg.html(message);
  dialog.showModal();
  dialog.on("close", function () {
    dialog.off(); dialog_op_confirm_action(dialog, action, options);
  });
}

function confirm_prompt(message, action, extra_opt) {
  return dialog_confirm_prompt(DIALOG, message, action, extra_opt);
}

function dialog_op_submit(dialog) {
  dialog.data("action", "submit");
}

function dialog_op_cancel(dialog) {
  dialog.data("action", "cancel");
  dialog.close();
}

function dialog_op_confirm_action(dialog, action, options) {
  var dialog_action = dialog.data("action");
  if (dialog_action == "submit") {
    action(options['data']);
  } else if (dialog_action == "cancel") {
    console.log("Action cancelled by user.");
    if (options['onabort']) options['onabort'](options['data']);
  } else {
    console.warn(`Unexpected dialog close action: ${dialog_action}`);
  }
}

$(function () {
  console.log(`ZW_Base.js version ${ZW_BASE_VERSION} initializing...`);

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

  DIALOG = $("#dialog");
  $("dialog").each(function (idx, elem) {
    const dialog = $(elem);
    dialog.find("input[type=submit]").click(
      function () { dialog_op_submit(dialog); });
    dialog.find("input[type=reset]").click(
      function () { dialog_op_cancel(dialog); });
  });
});