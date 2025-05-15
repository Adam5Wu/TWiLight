var DEVMODE = window.location.protocol === 'file:';

const URL_TWILIGHT_SETUP = "/!twilight/setup";
const PARAM_NUM_PIXELS = "num_pixels";
const PARAM_TEST_TRANSITION = "test_transition";

const DEBUG_CONFIG = {
  "num_pixels": "128",
  "transitions": {
    "dawn/dusk": {
      "type": "color-wipe",
      "color": "#736220",
      "duration_ms": "3000",
      "blade_width": "0.3",
      "direction": "LtR"
    },
    "off": {
      "type": "uniform-color",
      "color": "#000000",
      "duration_ms": "1000",
    }
  }
};

const TWILIGHT_CONFIG_UPDATE_DELAY = 2000;
const TWILIGHT_CONFIG_KEY_MAX_LEN = 20;
const TWILIGHT_DEFAULT_STRIP_LEN = 16;

const TWILIGHT_DEFAULT_COLOR = "#000000";
const TWILIGHT_DEFAULT_DURATION_MS = 3000;

var new_config = {};
var cur_config = {};

var config_update_timer = null;

function config_update_sync() {
  var strip_length = new_config['num_pixels'];
  if (cur_config['num_pixels'] != strip_length) {
    console.log("Update strip length:", strip_length);

    if (DEVMODE) {
      cur_config['num_pixels'] = strip_length;
      setTimeout(config_update_sync, 0);
      return;
    }

    probe_url_for(URL_TWILIGHT_SETUP + '?' + $.param({ [PARAM_NUM_PIXELS]: strip_length }),
      function () {
        cur_config['num_pixels'] = strip_length;
        setTimeout(config_update_sync, 0);
      },
      function (text) {
        console.log("Failed to update strip length:", text);
        strip_length = cur_config['num_pixels'];
        $("#strip-length-slider").slider("value", strip_length);
        new_config['num_pixels'] = strip_length;
        setTimeout(config_update_sync, 0);
      });
    return;
  }

  const transitions = new_config['transitions'];
  if (!_.isEqual(cur_config['transitions'], transitions)) {
    console.log("Update transitions:", transitions);

    //...
    cur_config['transitions'] = $.extend(true, {}, transitions);
    setTimeout(config_update_sync, 0);
    return;
  }

  //...

  console.log("Config sync complete");
  $("#config-form").removeClass("syncing");
  $("#btn-save").removeAttr("disabled");
}

function config_update_probe() {
  if (config_update_timer) {
    clearTimeout(config_update_timer);
    config_update_timer = null;
  }

  if (_.isEqual(new_config, cur_config)) {
    $("#btn-save").removeAttr("disabled");
    return;
  }

  $("#btn-save").attr("disabled", "disabled");
  config_update_timer = setTimeout(function () {
    config_update_timer = null;
    $("#config-form").addClass("syncing");
    console.log("Config sync start");
    config_update_sync();
  }, TWILIGHT_CONFIG_UPDATE_DELAY);
}

function get_fg_color(in_color) {
  var result = new Color(in_color);
  var l_value = result.oklch.l;
  if (l_value < 0.75) result.oklch.l = 0.6 + (0.53 * l_value);
  else result.oklch.l = l_value - 0.25;
  var c_value = result.oklch.c;
  if (c_value > 0.01 && c_value < 0.1) result.oklch.c += 0.1;
  return result.toString({ format: 'hex' });
}

function print_transition_params(transition) {
  if (!('duration_ms' in transition)) return "(unspecified)";

  var text = `Duration: ${(transition['duration_ms'] / 1000).toFixed(1)}s`;

  if ('color' in transition) {
    var bg_color = transition['color'];
    var fg_color = get_fg_color(bg_color);
    text += `<br>Color:
    <span style="color:${fg_color};background-color:${bg_color};border-radius:1em">
    &nbsp;${bg_color}&nbsp;</span>`;
  }

  if (transition['type'] == 'color-wipe') {
    text += "<br>Direction: ";

    var dir = transition['direction'];
    if (dir == 'LtR') text += 'Start->End';
    else if (dir == 'RtL') text += 'Start<-End';
    else text += `(unrecognized)`;

    text += `<br>Blade width: ${(transition['blade_width'] * 100).toFixed(1)}%`;
  }

  return text;
}

function config_received(config) {
  new_config = $.extend(true, {}, cur_config = config || {});

  // Populate widgets from config
  if ('num_pixels' in cur_config) {
    var strip_length_slider = $("#strip-length-slider");
    strip_length_slider.slider("value", cur_config['num_pixels']);
    strip_length_slider.find("input").val(cur_config['num_pixels']);
  }

  const transition_list = $("#strip-transition-list");
  transition_list.empty();
  if ('transitions' in cur_config) {
    for (var name in cur_config['transitions']) {
      const transition = cur_config['transitions'][name];
      const transition_row = $(`
      <tr>
        <td class="transition-name">${name}</td>
        <td class="transition-type">${transition['type']}</td>
        <td class="transition-params">${print_transition_params(transition)}</td>
        <td class="list-remove"></td>
      </tr>`);
      transition_row.data('name', name);
      transition_row.data('transition', transition)
      transition_list.append(transition_row);
    }
  } else {
    cur_config['transitions'] = {};
    new_config['transitions'] = {};
  }
  transition_list.append($(`
    <tr id="new-transition">
      <td class="list-append"></td>
    </tr>`));

//...
}

function enter_setup() {
  probe_url_for(URL_TWILIGHT_SETUP + '?' + $.param({ "state": "enter" }),
    function () {
      $("#config-form").removeClass("syncing");
    }, function (text) {
      notify_prompt(`<p>Enter setup mode failed:<br>${text}
        <p>Click OK to try again.`, enter_setup);
  });
}

function exit_setup_discard() {
  if (config_update_timer) {
    clearTimeout(config_update_timer);
    config_update_timer = null;
    $("#btn-save").removeAttr("disabled");
  }

  if (DEVMODE) return config_received($.extend(true, {}, DEBUG_CONFIG));

  $("#config-form").addClass("syncing");
  probe_url_for(URL_TWILIGHT_SETUP + '?' + $.param({ "state": "exit-discard" }),
    function () {
      block_prompt("<p>Set up discarded.<p>Reloading configuration...");
      probe_url_for(URL_TWILIGHT_SETUP, function (payload) {
        config_received(payload['config']);
        notify_prompt("<p>Click OK to enter setup mode...", enter_setup);
      }, function (text) {
        block_prompt(`<p><center>TWiLight unavailable:<p>${text}</center>`);
      });
    }, function (text) {
      $("#config-form").removeClass("syncing");
      notify_prompt(`<p>Unable to discard setup:<br>${text}`);
  });
}

function exit_setup_save() {
  $("#config-form").addClass("syncing");
  probe_url_for(URL_TWILIGHT_SETUP + '?' + $.param({ "state": "exit-save" }),
    function () {
      notify_prompt(`<p>Set up saved.
        <p>Click OK to enter setup mode...`, enter_setup);
    }, function (text) {
      $("#config-form").removeClass("syncing");
      notify_prompt(`<p>Unable to save setup:<br>${text}`);
  });
}

function strip_length_update(value) {
  new_config['num_pixels'] = value.toString();
  config_update_probe();
}

function remove_transition(transition_name) {
  const transitions = new_config['transitions'];
  if (transition_name in transitions) {
    const transition_list = $("#strip-transition-list").find("tr");
    for (var row_idx in transition_list) {
      var row = $(transition_list[row_idx]);
      if (row.data('name') == transition_name) {
        row.remove();
        break;
      }
    }
    delete transitions[transition_name];
    console.log(`Removed transition: '${transition_name}'`);
    config_update_probe();
  } else {
    console.log(`Transition '${transition_name}' not found.`);
  }
}

function rename_transition(old_name, new_name, row, cell, abort_action) {
  if (new_name.length == 0) {
    notify_prompt("<p>Transition name cannot be empty.", abort_action);
    return false;
  }

  if (new_name != old_name) {
    const transitions = new_config['transitions'];
    if (new_name in transitions) {
      notify_prompt(`<p>Transition name '${new_name}' already exists.`, abort_action);
      return false;
    }

    if (old_name.length > 0) {
      console.log(`Transition '${old_name}' --> '${new_name}'`);
      transitions[new_name] = transitions[old_name];
      delete transitions[old_name];
    } else {
      console.log(`New transition '${new_name}'`);
      transitions[new_name] = {};
    }
    row.data('name', new_name);
    cell.text(new_name);
    config_update_probe();
  }
  return true;
}

function change_transition_type(name, new_type, row, cell) {
  const transitions = new_config['transitions'];
  if (name in transitions) {
    const transition = transitions[name];
    transition['type'] = new_type;
    // Clear out transition-specific parameters
    for (var key in transition) {
      if (key == 'name' || key == 'type') continue;
      delete transition[key];
    }
    cell.text(new_type);
    row.find(".transition-params").html(print_transition_params(transition));
    row.data('transition', transition);
    config_update_probe();
  } else {
    console.log(`Transition '${name}' not found.`);
  }
}

function transition_params_test(dialog) {
  const test_button = dialog.find('.test-button');
  test_button.attr('disabled', 'disabled');

  const transition = dialog.data('collect-params')();
  console.log("Testing transition parameters:", transition);

  dialog.data('test-prober', probe_url_for(
    URL_TWILIGHT_SETUP + '?' + $.param({ [PARAM_TEST_TRANSITION]: JSON.stringify(transition) }),
    function () { console.log(`Sent transition params for testing`); },
    function (text) { notify_prompt(`<p>Failed to test transition params:<br>${text}`); },
    {
      always: function () {
        test_button.removeAttr('disabled');
        dialog.data('test-prober', null);
      }
    }
  ));
}

function transition_setup_uniform_color(row, cell, transition) {
  const transition_name = row.data('name');
  console.log(`Uniform-color transition setup for '${transition_name}'`);
  console.assert(transition['type'] == 'uniform-color', `Unexpected transition type: ${transition['type']}`);

  const dialog = $("#transition-params\\:uniform-color");
  const duration_slider = dialog.find("#uniform-color\\:duration-slider");
  const color_picker = dialog.find("#uniform-color\\:color-picker").data("color-picker");

  const duration = (transition['duration_ms'] || TWILIGHT_DEFAULT_DURATION_MS) / 1000;
  duration_slider.slider("value", duration);

  const color = transition['color'] || TWILIGHT_DEFAULT_COLOR;
  color_picker.color.set(color);

  const collect_params = function () {
    var new_params = $.extend(true, {}, transition);
    new_params['duration_ms'] = (duration_slider.slider("value") * 1000).toFixed(0);
    new_params['color'] = color_picker.color.hexString;
    return new_params;
  };
  dialog.data('collect-params', collect_params);

  dialog_confirm_prompt(dialog, transition_name, function () {
    var new_transition = collect_params();
    if (!_.isEqual(new_transition, transition)) {
      row.data('transition', new_transition);
      cell.html(print_transition_params(new_transition));
      new_config['transitions'][transition_name] = new_transition;
      config_update_probe();
    }
  }, {
    onabort: function () {
      const test_prober = dialog.data('test-prober');
      if (test_prober) { test_prober.abort(); }
    }
  });
}

function transition_setup_color_wipe(row, cell, transition) {

}

function transition_setup_color_wheel(row, cell, transition) {

}

const TRANSITIONS_SETUP = {
  "uniform-color": transition_setup_uniform_color,
  "color-wipe": transition_setup_color_wipe,
  "color-wheel": transition_setup_color_wheel
};

function transition_edit_name(row, cell) {
  const row_name = row.data('name');

  var input = $(`<textarea class="inline-edit"></textarea>`);
  input.text(cell.text());
  input.attr('maxlength', TWILIGHT_CONFIG_KEY_MAX_LEN);
  cell.append(input);
  input.focus();
  input.select();
  input.on('click', function (evt) { evt.stopPropagation(); });

  var abort_edit_exit = function () {
    input.data('op', null);
    input.focus();
  };
  var save_exit_edit = function (value) {
    input.data('op', 'save-exit');
    if (rename_transition(row_name, value, row, cell, abort_edit_exit)) {
      input.remove();
      return true;
    }
    return false;
  }
  var discard_exit_edit = function () {
    input.remove();
    // Check if we are abort inserting a new row
    if (row_name.length == 0) row.remove();
  };
  input.on('keydown', function (evt) {
    if (evt.which == 27) {
      // Escape leaves editing mode without saving
      discard_exit_edit();
    }
    else if (evt.which == 13) {
      evt.preventDefault();
      // Save and leave editing
      save_exit_edit(this.value);
    } else if (evt.which == 9) {
      evt.preventDefault();
      // Save and leave editing
      if (save_exit_edit(this.value)) {
        // Tab moves editing to the next cell
        transition_edit_type(row, cell.next());
      }
    }
  });
  var normalize_text = function (text, selection) {
    const leading_spaces = text.match(/^\s*/);
    const trailing_spaces = text.match(/\s*$/);
    const leading_space_len = leading_spaces ? leading_spaces[0].length : 0;
    const trailing_space_len = trailing_spaces ? trailing_spaces[0].length : 0;

    // Leading and trailing whitespaces are prune
    var trim_selection_start = Math.max(selection[0] - leading_space_len, 0);
    var trim_selection_from_end = Math.max(text.length - selection[1] - trailing_space_len, 0);
    text = text.trim();

    const before_selection_text = text.slice(0, trim_selection_start);
    const selection_text = text.slice(trim_selection_start, text.length - trim_selection_start - trim_selection_from_end);
    const after_selection_text = text.slice(text.length - trim_selection_from_end);

    const norm_before_selection_text = before_selection_text.replace(/\s+/g, ' ');
    var norm_after_selection_text = after_selection_text.replace(/\s+/g, ' ');
    var norm_selection_text = selection_text.replace(/\s+/g, ' ');

    // Removing leading space from `norm_selection_text` if `norm_before_seletion_text` already ends with a space
    if (norm_before_selection_text.endsWith(' ') && norm_selection_text.startsWith(' ')) {
      norm_selection_text = norm_selection_text.slice(1);
    }

    if (norm_selection_text.length == 0) {
      // Removing leading space from `norm_after_selection_text` if `norm_before_seletion_text` already ends with a space
      if (norm_before_selection_text.endsWith(' ') && norm_after_selection_text.startsWith(' ')) {
        norm_after_selection_text = norm_after_selection_text.slice(1);
      }
    } else {
      // Removing trailing space from `norm_selection_text` if `norm_after_selection_text` already starts with a space
      if (norm_after_selection_text.startsWith(' ') && norm_selection_text.endsWith(' ')) {
        norm_selection_text = norm_selection_text.slice(0, -1);
      }
    }

    const norm_text = norm_before_selection_text + norm_selection_text + norm_after_selection_text;
    selection[0] = norm_before_selection_text.length;
    selection[1] = Math.max(norm_text.length - norm_after_selection_text.length, norm_before_selection_text.length);
    return norm_text;
  }
  input.on('input', function () {
    const input_text = this.value;
    var selection = [this.selectionStart, this.selectionEnd];
    const norm_text = normalize_text(input_text, selection);
    if (input_text != norm_text) {
      this.value = norm_text;
      this.setSelectionRange(selection[0], selection[1]);
    }
  });
  input.on('blur', function () {
    if (input.data('op')) return;
    // Save and leave editing
    save_exit_edit(this.value);
  });
}

function transition_edit_type(row, cell) {
  const row_name = row.data('name');
  const transition = row.data('transition');

  var select = $(`<select class="inline-edit"></select>`);
  select.attr('size', Object.keys(TRANSITIONS_SETUP).length);
  var current_option = null;
  for (var type in TRANSITIONS_SETUP) {
    var option = $(`<option></option>`);
    option.text(type);
    if (transition['type'] == type) current_option = option;
    select.append(option);
  }
  cell.append(select);
  if (current_option) {
    current_option.attr('selected', 'selected');
    current_option.css('font-weight', 'bolder');
    current_option.css('text-decoration', 'underline');
    current_option[0].scrollIntoView();
  }
  select.focus();
  // Stabilizes row height
  row.css("height", Math.ceil(row.height()) + "px");

  const remove_select = function () {
    row.css("height", "0");
    select.remove();
  };
  const confirm_transition_type_change = function (select, confirm_action) {
    select.data('op', 'confirm');
    const selected = select.val();
    if (!selected) {
      notify_prompt("<p>Please select a transition type.", function () {
        select.data('op', null);
        select.focus();
      });
      return;
    }

    if (selected == transition['type']) {
      confirm_action();
      return;
    }

    const apply_transition_type_change = function () {
      change_transition_type(row_name, selected, row, cell);
      confirm_action();
    };
    if (!('duration_ms' in transition)) {
      apply_transition_type_change();
      return;
    }
    confirm_prompt("<p>Change transition type?<br>(This will erase current parameters.)",
      apply_transition_type_change, {
      onabort: function () {
        select.data('op', null);
        select.focus();
      }
    });
  };
  select.on('click', function (evt) { evt.stopPropagation(); });
  select.on('keydown', function (evt) {
    if (evt.which == 27) {
      // Restore the current transition display
      const selected = select.find('option:selected').val();
      if (selected != transition['type'] && transition['type']) {
        row.find('.transition-params').html(print_transition_params(transition));
      }
      remove_select();
    } else if (evt.which == 13) {
      evt.preventDefault();
      // Confirm and save transition type
      confirm_transition_type_change(select, function () {
        // Exit editing
        remove_select();
      });
    } else if (evt.which == 9) {
      evt.preventDefault();
      // Confirm and save transition type
      confirm_transition_type_change(select, function () {
        // Moves editing to other cells
        remove_select();
        if (evt.shiftKey) transition_edit_name(row, cell.prev());
        else transition_edit_params(row, cell.next());
      });
    }
  });
  select.on('blur', function () {
    if (select.data('op')) return;
    // Confirm and save transition type, then exit editing
    confirm_transition_type_change(select, function () {
      // Exit editing
      remove_select();
    });
  });
  select.on('change', function () {
    const selected = select.find('option:selected').val();
    console.log(`Selected transition type: ${selected}`);
    if (selected == transition['type']) {
      row.find('.transition-params').html(print_transition_params(transition));
    } else {
      row.find('.transition-params').text("(to be specified)");
    }
  });
}

function transition_edit_params(row, cell) {
  const transition = row.data('transition');
  const transition_type = transition['type'];

  if (!transition_type) {
    transition_edit_type(row, cell.prev());
    return;
  }

  if (transition_type in TRANSITIONS_SETUP) {
    TRANSITIONS_SETUP[transition_type](row, cell, transition);
  } else {
    console.log(`Unexpected transition type: ${transition_type}`);
  }
}

function click_transition_cell(evt) {
  var cell = $(evt.target);
  const row = cell.parent();

  if (cell.hasClass('list-append')) {
    console.log("Clicked on append row");

    var new_row = $(`
    <tr>
      <td class="transition-name">new-transition</td>
      <td class="transition-type">(to be specified)</td>
      <td class="transition-params">(to be specified)</td>
      <td class="list-remove"></td>
    </tr>`);
    new_row.data('name', '');
    new_row.data('transition', {})
    row.before(new_row);

    transition_edit_name(new_row, new_row.find(".transition-name"));
  } else {
    const row_name = row.data('name');
    console.log(`Clicked on transition row: '${row_name}'`);

    if (cell.hasClass('list-remove')) {
      confirm_prompt("<p>Remove this transition?", remove_transition, { data: row_name });
    } else if (cell.hasClass('transition-name')) {
      transition_edit_name(row, cell);
    } else if (cell.hasClass('transition-type')) {
      transition_edit_type(row, cell);
    } else if (cell.hasClass('transition-params')) {
      transition_edit_params(row, cell);
    } else {
      console.log(`Unexpected cell clicked: ${cell.html()}`);
    }
  }
}

function setup_num_slider(slide_bar, val_config, onchange) {
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
  setup_num_input(slide_input, {
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

function setup_num_input(input, options, events) {
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
      $(this).blur();
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

function setup_cc_input(input, picker, ctype, ccstr, options) {
  setup_num_input(input,
    Object.assign(options, {
      spinctrl: true, min: 0,
    }), {
    oninput: function (cctext, ccval) {
      if (typeof ccval == 'number') {
        picker.color.setChannel(ctype, ccstr, ccval);
      }
    },
    onblur: function () {
      if (!this.value) {
        $(this).data('refresh')(picker.color[ctype][ccstr]);
      }
    }
  });
}

function setup_color_picker(selector) {
  const picker = new iro.ColorPicker(selector, {
    width: 180, sliderSize: 12,
    layout: [{
      component: iro.ui.Slider,
    }, {
      component: iro.ui.Wheel,
    }]
  });
  $(selector).data("color-picker", picker);

  const input_rgb = $("#uniform-color\\:color-picker\\:input-rgb");
  const input_cc_r = $("#uniform-color\\:color-picker\\:input-red");
  const input_cc_g = $("#uniform-color\\:color-picker\\:input-green");
  const input_cc_b = $("#uniform-color\\:color-picker\\:input-blue");
  const input_cc_h = $("#uniform-color\\:color-picker\\:input-hue");
  const input_cc_s = $("#uniform-color\\:color-picker\\:input-sat");
  const input_cc_v = $("#uniform-color\\:color-picker\\:input-val");

  setup_num_input(input_rgb, {
    hex: true, fixedlen: 6
  }, {
    oninput: function (colortext, colorval) {
      picker.color.set('#' + colortext);
    }
  });
  setup_cc_input(input_cc_r, picker, 'rgb', 'r', { max: 255, pagesize: 16 });
  setup_cc_input(input_cc_g, picker, 'rgb', 'g', { max: 255, pagesize: 16 });
  setup_cc_input(input_cc_b, picker, 'rgb', 'b', { max: 255, pagesize: 16 });
  setup_cc_input(input_cc_h, picker, 'hsv', 'h', { max: 360, pagesize: 15, modulo: 360, fixedpoint: 1 });
  setup_cc_input(input_cc_s, picker, 'hsv', 's', { max: 100, pagesize: 10, fixedpoint: 1 });
  setup_cc_input(input_cc_v, picker, 'hsv', 'v', { max: 100, pagesize: 10, fixedpoint: 1 });

  picker.on("color:change", function (color) {
    input_rgb.data('refresh')(color.hexString.substring(1));
    input_cc_r.data('refresh')(color.rgb.r);
    input_cc_g.data('refresh')(color.rgb.g);
    input_cc_b.data('refresh')(color.rgb.b);
    input_cc_h.data('refresh')(color.hsv.h);
    input_cc_s.data('refresh')(color.hsv.s);
    input_cc_v.data('refresh')(color.hsv.v);
  });
}

$(function () {
  block_prompt("<p><center>Fetching configuration...</center>");

  //--------------------
  // Initialize setup page elements

  // -- Strip config
  setup_num_slider($("#strip-length-slider"), {
    min: 1, max: 1024, pagecount: 64,
    default: TWILIGHT_DEFAULT_STRIP_LEN,
  }, strip_length_update);

  $("#strip-transition-list").on("click", "td", click_transition_cell);

  $("#config-sections").accordion({
    heightStyle: 'content',
    activate: function (event, ui) {
      ui.newPanel.find('[tabindex], input').first().focus();
    }
  });

  // -- Transition params config dialogs
  setup_num_slider($("#uniform-color\\:duration-slider"), {
    min: 0, max: 60, step: 0.1, pagecount: 60,
    default: 3, fixedpoint: 1
  });
  setup_color_picker("#uniform-color\\:color-picker");

  //...

  // Hook the test buttons for all dialogs
  $("dialog.config").each(function (index, element) {
    const dialog = $(element);
    dialog.find(".dlg-ctrl>input.test-button").click(function () {
      transition_params_test(dialog);
    });
  });

  //--------------------
  // Page control
  $("#btn-discard").click(function () {
    confirm_prompt("<p>Discard changes and exit setup mode?", exit_setup_discard);
  });
  $("#config-form").submit(function (evt) {
    evt.preventDefault();
    confirm_prompt("<p>Save changes and exit setup mode?", exit_setup_save);
  });

  if (!DEVMODE) {
    $("#strip-transition-list").empty();

    probe_url_for(URL_TWILIGHT_SETUP, function (payload) {
      config_received(payload['config']);
      if (payload['setup']) {
        prompt_close();
        $("#config-form").removeClass("syncing");
      } else {
        notify_prompt("<p>Click OK to enter setup mode...", enter_setup);
      }
    }, function (text) {
      block_prompt(`<p><center>TWiLight unavailable:<br>${text}</center>`);
    });
  } else {
    setTimeout(function () {
      setTimeout(function () {
        config_received($.extend(true, {}, DEBUG_CONFIG));
        prompt_close();
        $("#config-form").removeClass("syncing");
      }, 1000);
    }, 500);
  }
});