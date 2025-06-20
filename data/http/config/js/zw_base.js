const ZWBASE_VERSION = "1.1.9"

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
      if (options.onabort) options.onabort(options.data);
      if (options.always) options.always(options.data);
    },
    action: function () {
      const action_attempt = ++this.attempt;
      $.when(this.xhr).then(function (payload) {
        console.log(`Probe '${url}' succeeded:`, payload);
        success_action(payload, options.data);
        if (options.always) options.always(options.data);
      }, function (jqXHR, textStatus) {
        // Check if the probing has been aborted
        if (!prob_container['xhr']) {
          console.log(`Probe '${url}' aborted.`);
          return;
        }

        var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
        console.log(`Probe '${url}' failed (#${prob_container['attempt']}): ${resp_text || textStatus}`);

        // Keep retrying if no response text (likely timed out)
        if (resp_text || action_attempt >= options.retry) {
          fail_action(resp_text || `Request failed after ${action_attempt} attempts.`, ['data']);
          if (options.always) options.always(options.data);
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

function get_locale_weekday_map(formatter) {
  var weekdays = {};
  for (var idx = 0; idx < 7; idx++) {
    const sample_time = new Date(Date.UTC(2025, 0, 10 + idx));
    const sample_text = formatter.format(sample_time);
    weekdays[sample_time.getDay()] = sample_text;
  }
  return weekdays;
}

function get_locale_weekday_list(locale, style = "short") {
  const formatter = Intl.DateTimeFormat(locale, {
    calendar: "gregory", weekday: style
  });
  const weekdays = get_locale_weekday_map(formatter);

  var weekday_list = [];
  const locale_obj = new Intl.Locale(formatter.resolvedOptions().locale);
  const first_dow = locale_obj.getWeekInfo().firstDay;
  for (var idx = 0; idx < 7; idx++) {
    const dow = (first_dow + idx) % 7;
    weekday_list.push([dow, weekdays[dow]]);
  }
  return weekday_list;
}

function get_locale_month_list(locale, style = "short") {
  const formatter = Intl.DateTimeFormat(locale, {
    calendar: "gregory", month: style
  });

  var month_list = [];
  for (var idx = 0; idx < 12; idx++) {
    const sample_time = new Date(Date.UTC(2025, idx, 15));
    const sample_text = formatter.format(sample_time);
    month_list.push(sample_text);
  }
  return month_list;
}

function enable_select_wheeling(container) {
  container.on("wheel", function (evt) {
    if (document.activeElement == this) {
      evt.preventDefault();
      evt.stopPropagation();
      const cur_sel = this.selectedIndex;
      var new_sel = cur_sel;
      if (cur_sel < 0) new_sel = 0;
      else if (evt.originalEvent.deltaY > 0) {
        for (var idx = cur_sel + 1; idx < this.length; idx++) {
          if (!this.options[idx].disabled) {
            new_sel = idx; break;
          }
        }
      } else {
        for (var idx = cur_sel - 1; idx >= 0; idx--) {
          if (!this.options[idx].disabled) {
            new_sel = idx; break;
          }
        }
      }
      if (cur_sel !== new_sel) {
        this.selectedIndex = new_sel;
        $(this).trigger('change');
      }
    }
  });
}

//------------------------------
// Input control setup

function input_setup_num_slider(slide_bar, val_config, onchange) {
  slide_bar.data('val_config', val_config);

  const val_min = val_config['min'] || 0;
  const val_max = val_config['max'] || 100;
  const val_def = val_config['default'] || ((val_min + val_max) / 2);
  const val_step = val_config['step'] || 1;
  const fixedpoint = val_config['fixedpoint'] || 0;
  const pagecount = val_config['pagecount'] || 10;

  const slide_handle = slide_bar.find(".ui-slider-handle");
  const slide_input = slide_handle.find("input");
  slide_input.val(val_def.toFixed(fixedpoint));
  slide_input.attr("maxlength", val_max.toFixed(fixedpoint).length);

  slide_bar.slider({
    // animate: true,
    min: val_min,
    max: val_max,
    value: val_def,
    step: val_step,
    pagecount: pagecount,
    slide: function (event, ui) {
      slide_input.data('refresh')(ui.value);
    },
    change: function (event, ui) {
      slide_input.data('refresh')(ui.value);
      if (typeof onchange == 'function') onchange(ui.value);
    }
  });
  slide_bar.on('wheel', function (evt) {
    evt.preventDefault();
    evt.stopPropagation();

    const delta = evt.originalEvent.deltaY;
    const val = slide_bar.slider("value");
    const new_val = val + (delta > 0 ? -val_step : val_step);
    slide_bar.slider("value", new_val);
  });
  slide_bar.find(".ui-slider-handle").on('dblclick', function () {
    slide_input.focus();
    slide_input.select();
  });
  input_setup_numeric(slide_input, {
    fixedpoint: fixedpoint, min: val_min, max: val_max
  }, {
    onconfirm: function (val) {
      if (typeof val == 'number') {
        slide_bar.slider("value", val);
      }
    },
    onblur: function () {
      slide_input.data('refresh')(slide_bar.slider("value"));
    }
  });
  if (!slide_handle.attr("title")) {
    slide_handle.attr("title", slide_input.attr("title"));
  }
}

function input_setup_numeric(input, options, events) {
  input.data('options', options);

  const hex = options['hex'] || false;
  const negative = options['negative'] || false;
  const fixedpoint = options['fixedpoint'] || 0;
  const spinctrl = options['spinctrl'] || false;
  const stepsize = options['stepsize'] || 1;
  const pagesize = options['pagesize'] || 0;
  const minval = ('min' in options) ? options['min'] : null;
  const maxval = ('max' in options) ? options['max'] : null;
  const modulo = options['modulo'] || 0;
  const fixedlen = options['fixedlen'] || 0;

  if (typeof minval == 'number' && typeof maxval == 'number') {
    const range = maxval - minval;
    console.assert(range >= 0, "Invalid ['minval', 'maxval'] range");
  }
  if (modulo && (typeof minval == 'number' || typeof maxval == 'number')) {
    const mod_min = minval || 0;
    if (typeof minval != 'number') {
      console.log("Modulo with `maxval` only, assume `minval` of 0");
    }
    const mod_max = maxval || 0;
    if (typeof maxval != 'number') {
      console.log("Modulo with `minval` only, assume `maxval` of 0");
    }
    const mod_range = mod_max - mod_min;
    console.assert(mod_range == modulo, "Modulo match the value range");
  }
  console.assert(!(fixedpoint && hex), "Incompatible mode: 'hex' and 'fixedpoint'");
  if (fixedlen) {
    console.assert((typeof minval != 'number') && (typeof maxval != 'number'),
      "Fixed length input mode is incompatible with range specifiers");
    console.assert(!negative, "Fixed length input mode is incompatible with negative input");
  }

  if (!input.attr('title')) {
    var hint = "Input "
    if (hex) {
      hint += "a hexadecimal number";
      if (typeof minval == 'number') {
        if (typeof maxval == 'number') {
          hint += ` between [${minval.toString(16)}, ${maxval.toString(16)}]`;
        } else {
          hint += ` >= ${minval.toString(16)}`;
        }
      } else if (typeof maxval == 'number') {
        hint += ` <= ${maxval.toString(16)}`;
      }
    } else {
      hint += fixedpoint ? "a number" : "an integer";
      if (typeof minval == 'number') {
        if (typeof maxval == 'number') {
          hint += ` between [${minval.toFixed(fixedpoint)}, ${maxval.toFixed(fixedpoint)}]`;
        } else {
          hint += ` >= ${minval.toFixed(fixedpoint)}`;
        }
      } else if (typeof maxval == 'number') {
        hint += ` <= ${maxval.toFixed(fixedpoint)}`;
      }
    }
    input.attr('title', hint);
  }
  if (fixedlen) {
    input.attr('maxlength', fixedlen);
  }

  var normalize_text = function (text) {
    // Truncate excessive precisions
    if (fixedpoint) {
      const parts = text.split('.', 2);
      if (parts.length > 1 && parts[1].length > fixedpoint) {
        text = parts[0] + '.' + parts[1].substring(0, fixedpoint);
      }
    }
    if (fixedlen) text = text.padStart(fixedlen, '0');
    return text;
  }

  var parse_number = function (text) {
    const num_text = text || 0;
    const num_val = Number(hex ? "0x" + num_text : num_text);
    return isNaN(num_val) ? 0 : num_val;
  };

  var normalize_val = function (new_val) {
    if (modulo) {
      if (typeof minval == 'number' || typeof maxval == 'string') {
        // Work on rounded number to ensure the fixed point string won't touch maxval
        if (fixedpoint) new_val = parseFloat(new_val.toFixed(fixedpoint));
        if (typeof minval == 'number') while (new_val < minval) new_val += modulo;
        if (typeof maxval == 'number') while (new_val >= maxval) new_val -= modulo;
      } else new_val %= modulo;
    } else {
      if (typeof minval == 'number' && new_val < minval) new_val = minval;
      if (typeof maxval == 'number' && new_val > maxval) new_val = maxval;
    }
    return new_val;
  };

  var marshal_number = function (value) {
    var text = hex ? value.toString(16) : value.toFixed(fixedpoint);
    if (fixedlen) text = text.padStart(fixedlen, '0');
    return text;
  }

  var refresh_input = function (new_text, new_val, no_trigger) {
    const input_text = this.value;
    const selection = [this.selectionStart, this.selectionEnd];

    if (!new_text) new_text = marshal_number(new_val);
    this.value = new_text;

    // Restore the cursor
    const input_ipart_len = input_text.split('.', 2)[0].length;
    const norm_ipart_len = new_text.split('.', 2)[0].length;
    const new_selection_start = Math.min(new_text.length, (Math.max(0,
      norm_ipart_len - (input_ipart_len - selection[0]))));
    const new_selection_end = Math.min(new_text.length, (Math.max(0,
      norm_ipart_len - (input_ipart_len - selection[1]))));
    this.setSelectionRange(new_selection_start, new_selection_end);

    if (!no_trigger && typeof events['oninput'] == 'function') {
      events['oninput'].apply(this, [new_text, new_val]);
    }
  };
  // Export this function for external value assignment
  input.data("refresh", function (val) {
    var norm_val;
    if (typeof val == 'number') {
      norm_val = normalize_val(val);
    } else {
      const norm_text = normalize_text(val);
      norm_val = normalize_val(parse_number(norm_text));
    }
    input.each(function () { refresh_input.apply(this, [null, norm_val, true]); });
  });

  var derive_selection = function () {
    const text_len = this.value.length;
    const selection = [this.selectionStart, this.selectionEnd];
    if (selection[1] - selection[0] != 1) {
      if (selection[0] < text_len) this.selectionEnd = selection[0] + 1;
      else this.selectionStart = selection[1] - 1;
    }
  };

  if (fixedlen) {
    input.val("".padStart(fixedlen, '0'));
    input.on('focus', derive_selection);
    input.on('click', function (evt) {
      setTimeout(function () {
        derive_selection.apply(this);
      }.bind(this), 0);
    });
  }

  input.on('keydown', function (evt) {
    // Prevent default behavior for keys other than the following situation
    if (!(
      // Regular 0~9, Numpad 0~9
      ($.inArray(evt.key, ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9']) != -1) ||
      // If negative input, '+' and '-'
      (negative && (evt.key == '+' || evt.key == '-')) ||
      // If hex input, a-f and A-F
      (hex && $.inArray(evt.key.toLowerCase(), ['a', 'b', 'c', 'd', 'e', 'f']) != -1) ||
      // If allow floats, '.' if not already has one
      (fixedpoint && !this.value.includes('.') && evt.key == '.') ||
      // Edit function keys: left, right, delete, backspace, home, end, tab (unfocus)
      ($.inArray(evt.which, [37, 39, 46, 8, 36, 35, 9]) != -1) ||
      // If holding Alt or Ctrl
      (evt.altKey || evt.ctrlKey))) {
      evt.preventDefault();
    }
    const input_text = this.value;
    const input_val = parse_number(input_text);

    // Handle negative / positive signs
    if (negative && (evt.key == '+' || evt.key == '-')) {
      evt.preventDefault();

      var new_val = input_val;
      if (evt.key == '+' && input_val < 0) {
        if (typeof maxval != 'number' || input_val <= maxval) {
          new_val = Math.abs(new_val);
        }
      } else if (evt.key == '-' && input_val > 0) {
        if (typeof minval != 'number' || input_val >= minval) {
          new_val = -Math.abs(new_val);
        }
      }
      if (new_val != input_val) refresh_input.apply(this, [null, new_val]);
    }
    // Handle spinner control (up, down, page-up, page-down)
    if (spinctrl && $.inArray(evt.which, [38, 40, 33, 34]) != -1) {
      evt.preventDefault();
      evt.stopPropagation();

      var new_val = input_val;
      if (evt.which == 38) new_val += stepsize;
      else if (evt.which == 40) new_val -= stepsize;
      else if (evt.which == 33) new_val += pagesize;
      else if (evt.which == 34) new_val -= pagesize;

      new_val = normalize_val(new_val);
      if (new_val != input_val) refresh_input.apply(this, [null, new_val]);
    }
    if (evt.which == 27) {
      // Esc removes the focus.
      dialog_element_unfocus($(this));
    } else if (evt.which == 13) {
      // Enter confirms the value
      if (typeof events['onconfirm'] == 'function') {
        const val = input_text ? input_val : null;
        events['onconfirm'].apply(this, [val]);
      }
    }

    // Do not bubble up cursor control keys:
    // Left, right, home, end
    if ($.inArray(evt.which, [37, 39, 36, 35]) != -1) {
      evt.stopPropagation();

      // Fixed length mode takes over regular cursor operation
      if (fixedlen && !evt.ctrlKey) {
        evt.preventDefault();
        const selection_start = this.selectionStart;
        if (evt.which == 37) {
          this.selectionStart = Math.max(selection_start - 1, 0);
        } else if (evt.which == 39) {
          this.selectionStart = Math.min(selection_start + 1, this.value.length);
        } else if (evt.which == 36) {
          this.selectionStart = 0;
        } else if (evt.which == 35) {
          this.selectionStart = this.value.length;
        }
      }
    }
    // Special handling for backspace and delete for fixed length mode
    if (fixedlen && $.inArray(evt.which, [8, 46]) != -1) {
      evt.preventDefault();
      evt.stopPropagation();

      const input_text = this.value;
      const selection = [this.selectionStart, this.selectionEnd];
      const new_text = input_text.slice(0, selection[0]) +
        "".padStart(selection[1] - selection[0], '0') +
        input_text.slice(selection[1]);
      const new_val = parse_number(new_text);
      refresh_input.apply(this, [null, new_val]);
    }
    // Fix selection (unless ctrl key is held)
    if (fixedlen && !evt.ctrlKey) derive_selection.apply(this);
  });

  if (spinctrl) {
    input.on('wheel', function (evt) {
      evt.preventDefault();
      evt.stopPropagation();

      const input_text = this.value;
      const input_val = parse_number(input_text);

      const delta = evt.originalEvent.deltaY;
      var new_val = input_val + (delta > 0 ? -stepsize : stepsize);

      new_val = normalize_val(new_val);
      if (new_val != input_val) refresh_input.apply(this, [null, new_val]);
    });
  }

  input.on('input', function () {
    const input_text = this.value;

    if (!input_text) {
      // Still notify the event
      if (typeof events['oninput'] == 'function') {
        events['oninput'].apply(this, ["", null]);
      }
      return;
    }

    const norm_text = normalize_text(input_text);
    const input_val = parse_number(norm_text);
    const norm_val = normalize_val(input_val);
    const norm_val_text = marshal_number(norm_val);

    if (norm_text != input_text || norm_val != input_val ||
      !norm_val_text.startsWith(input_text)) {
      // Just refresh the input value, we will trigger event later
      refresh_input.apply(this, [norm_val_text, norm_val, true]);
    }
    if (fixedlen) derive_selection.apply(this);

    if (typeof events['oninput'] == 'function') {
      events['oninput'].apply(this, [norm_text, norm_val]);
    }
  });
  if (events['onblur']) {
    input.on('blur', events['onblur']);
  }
}

//------------------------------
// Dialog utility functions
var DIALOG;

function dialog_element_unfocus(elem) {
  const dialog_parent = elem.parentsUntil("body", "dialog");
  if (dialog_parent.length > 0) dialog_parent.focus();
  else elem.blur();
}

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

  if (options.has_ok) {
    dialog_ok.show();
    dialog_ok.attr('value', options.ok_text || "OK");
  } else {
    dialog_ok.hide();
  }
  if (options.has_cancel) {
    dialog_cancel.show();
    dialog_cancel.attr('value', options.cancel_text || "Cancel");
  } else {
    dialog_cancel.hide();
  }

  // Clear extra controls from last use
  var extra_ctrl = dialog.data('extra_ctrl');
  if (extra_ctrl) {
    extra_ctrl.remove();
    dialog.data('extra_ctrl', null);
  }

  // Add extra controls for this use
  if (options.extra_ctrl) {
    extra_ctrl = options.extra_ctrl;
    dialog_ok.before(extra_ctrl);
    dialog.data('extra_ctrl', extra_ctrl);
  }

  // Clear left-over event handlers from last use.
  dialog.off();

  if (!options.esc_close) {
    dialog.on("keydown", function (evt) {
      if (evt.which == 27) evt.preventDefault();
    });
  }
  if (options.backdrop_close) {
    dialog.on("click", function (evt) {
      if (evt.target === evt.currentTarget) {
        dialog_prompt_close(dialog, true);
      }
    });
  }
  dialog.data('presubmit', options.presubmit);
}

function dialog_block_prompt(dialog, message, extra_opt) {
  const options = $.extend({
    has_ok: false, has_cancel: false
  }, extra_opt);
  dialog_op_setup(dialog, options);
  var dialog_msg = dialog.find(".dlg-message");
  dialog_msg.html(message);
  dialog.showModal();
}

function block_prompt(message, extra_opt) {
  return dialog_block_prompt(DIALOG, message, extra_opt);
}

function dialog_notify_prompt(dialog, message, action, extra_opt) {
  const options = $.extend({
    has_ok: true, has_cancel: false,
    esc_close: true, backdrop_close: true
  }, extra_opt);
  dialog_op_setup(dialog, options);
  var dialog_msg = dialog.find(".dlg-message");
  dialog_msg.html(message);
  dialog.showModal();
  dialog.on("close", function () {
    dialog.off();
    if (typeof action == 'function')
      action.apply(dialog, [options.data]);
  });
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
    dialog.off();
    dialog_op_confirm_action(dialog, action, options);
  });
}

function confirm_prompt(message, action, extra_opt) {
  return dialog_confirm_prompt(DIALOG, message, action, extra_opt);
}

function dialog_op_submit(elem, evt) {
  const dialog = $(elem);
  const presubmit = dialog.data("presubmit");
  if (presubmit && !presubmit.apply(dialog, [dialog.data("data")])) {
    console.log("Presubmit failed.");
    evt.preventDefault();
    return;
  }
  dialog.data("action", "submit");
}

function dialog_op_cancel(elem) {
  const dialog = $(elem);
  dialog.data("action", "cancel");
  elem.close();
}

function dialog_op_confirm_action(dialog, action, options) {
  var dialog_action = dialog.data("action");
  if (dialog_action == "submit") {
    action.apply(dialog, [options.data]);
  } else if (dialog_action == "cancel") {
    console.log("Action cancelled by user.");
    if (options.onabort)
      options.onabort.apply(dialog, [options.data]);
  }
  if (options.always)
    options.always.apply(dialog, [options.data]);
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
      function (evt) { dialog_op_submit(elem, evt); });
    dialog.find("input[type=reset]").click(
      function (evt) { dialog_op_cancel(elem, evt); });
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