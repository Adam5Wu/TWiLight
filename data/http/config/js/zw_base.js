const ZWBASE_VERSION = "1.1.0"

const URL_BOOT_SERIAL = "/!sys/boot_serial";

//----------------------------------------
// Utility functions
const PROBE_DEFAULT_RETRY = 5;
const PROBE_STATUS_INTERVAL = 3000;

function probe_url_for(url, success_action, fail_action, extra_opt) {
  var options = $.extend({
    retry: PROBE_DEFAULT_RETRY,
  }, extra_opt);
  var prob_container = {
    attempt: 0,
    timer: null,
    xhr: $.get(url),
    abort: function () {
      if (this.xhr) { this.xhr.abort(); this.xhr = null; }
      if (this.timer) clearTimeout(this.timer);
      if (options['onabort']) options['onabort'](options['data']);
      if (options['always']) options['always'](options['data']);
    },
    action: function () {
      const action_attempt = ++this.attempt;
      $.when(this.xhr).then(function (payload) {
        console.log(`Probe '${url}' succeeded:`, payload);
        success_action(payload, options['data']);
        if (options['always']) options['always'](options['data']);
      }, function (jqXHR, textStatus) {
        // Check if the probing has been aborted
        if (!prob_container['xhr']) {
          console.log(`Probe '${url}' aborted.`);
          return;
        }

        var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
        console.log(`Probe '${url}' failed (#${prob_container['attempt']}): ${resp_text || textStatus}`);

        // Keep retrying if no response text (likely timed out)
        if (resp_text || action_attempt >= options['retry']) {
          fail_action(resp_text || `Request failed after ${action_attempt} attempts.`, ['data']);
          if (options['always']) options['always'](options['data']);
          return;
        }

        prob_container['timer'] = setTimeout(function () {
          prob_container['timer'] = null;
          prob_container['xhr'] = $.get(url);
          prob_container['action'].apply(prob_container);
        }, PROBE_STATUS_INTERVAL);
      });
    }
  };

  prob_container.action();
  return prob_container;
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
  dialog.on("keydown", function (evt) {
    if (evt.which == 27) evt.preventDefault();
  });
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
  }
  if (options['always']) options['always'](options['data']);
}

$(function () {
  console.log(`ZW_Base.js version ${ZWBASE_VERSION} initializing...`);

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

  // Extend JQueryUI Slider to take `pagecount`
  if ($.ui) {
    $.widget("ui.slider", $.ui.slider, {
      _create: function () {
        this._super();
        this.numPages = this.options["pagecount"] || this.numPages;
      }
    });
  }
});