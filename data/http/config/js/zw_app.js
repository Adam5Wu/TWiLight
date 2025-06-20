var DEVMODE = window.location.protocol === 'file:';

const URL_TWILIGHT_SETUP = "/!twilight/setup";
const PARAM_NUM_PIXELS = "num_pixels";
const PARAM_TEST_TRANSITION = "test_transition";
const PARAM_SET_TRANSITIONS = "transitions";
const PARAM_SET_EVENTS = "events";

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
    "bright": {
      "type": "uniform-color",
      "color": "#ffffff",
      "duration_ms": "1000",
    },
    "off": {
      "type": "uniform-color",
      "color": "#000000",
      "duration_ms": "1000",
    }
  },
  "events": [{
    "type": "weekly",
    "daily": ["133", "1260"],
    "weekly": ["1", "4"],
    "transitions": ["bright", "off"]
  }, {
    "type": "annual",
    "daily": ["567", "866"],
    "annual": ["3", "6"],
    "transitions": ["dawn/dusk"]
  }, {
    "type": "daily",
    "daily": ["0"],
    "transitions": ["off"]
  }]
};

const TWILIGHT_CONFIG_UPDATE_DELAY = 2000;
const TWILIGHT_CONFIG_KEY_MAX_LEN = 20;
const TWILIGHT_DEFAULT_STRIP_LEN = 16;

const TWILIGHT_DEFAULT_COLOR = "#000000";
const TWILIGHT_DEFAULT_DURATION_MS = 3000;

var new_config = {};
var cur_config = {};

var config_update_timer = null;

function filter_incomplete_transitions(transitions) {
  var result = {};
  for (const key in transitions) {
    const transition = transitions[key];
    if ("duration_ms" in transition) {
      result[key] = structuredClone(transition);
    }
  }
  return result;
}

function config_update_sync(failure_cnt = 0) {
  const strip_length = new_config['num_pixels'];
  if (cur_config['num_pixels'] != strip_length) {
    console.log("Update strip length:", strip_length);

    if (DEVMODE) {
      cur_config['num_pixels'] = strip_length;
      setTimeout(config_update_sync, 100);
      return;
    }

    $.ajax({
      url: URL_TWILIGHT_SETUP + '?' + $.param({ [PARAM_NUM_PIXELS]: strip_length }),
    }).done(function () {
      cur_config['num_pixels'] = strip_length;
    }).fail(function (jqXHR, textStatus) {
      var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : textStatus;
      console.log("Failed to update strip length:", resp_text);
      ++failure_cnt;
      // TODO: Instead of forcibly losing data, prompt for retry or abort.
      new_config['num_pixels'] = cur_config['num_pixels'];
    }).always(function () {
      setTimeout(config_update_sync, 0, failure_cnt);
    });
    return;
  }

  const transitions = filter_incomplete_transitions(new_config['transitions']);
  if (!_.isEqual(cur_config['transitions'], transitions)) {
    console.log("Update transitions:", transitions);

    if (DEVMODE) {
      cur_config['transitions'] = structuredClone(transitions);
      setTimeout(config_update_sync, 100);
      return;
    }

    $.ajax({
      method: 'PUT',
      url: URL_TWILIGHT_SETUP + '?' + $.param({ [PARAM_SET_TRANSITIONS]: null }),
      data: JSON.stringify(transitions),
      contentType: 'application/json',
    }).done(function () {
      cur_config['transitions'] = structuredClone(transitions);
    }).fail(function (jqXHR, textStatus) {
      var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : textStatus;
      console.log("Failed to set transitions:", resp_text);
      ++failure_cnt;
      // TODO: Instead of forcibly losing data, prompt for retry or abort.
      new_config['transitions'] = structuredClone(cur_config['transitions']);
    }).always(function () {
      setTimeout(config_update_sync, 0, failure_cnt);
    });
    return;
  }

  const events = new_config['events'];
  if (!_.isEqual(cur_config['events'], events)) {
    console.log("Update events:", events);

    if (DEVMODE) {
      cur_config['events'] = structuredClone(events);
      setTimeout(config_update_sync, 100);
      return;
    }

    $.ajax({
      method: 'PUT',
      url: URL_TWILIGHT_SETUP + '?' + $.param({ [PARAM_SET_EVENTS]: null }),
      data: JSON.stringify(events),
      contentType: 'application/json',
    }).done(function () {
      cur_config['events'] = structuredClone(events);
    }).fail(function (jqXHR, textStatus) {
      var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : textStatus;
      console.log("Failed to set events:", resp_text);
      ++failure_cnt;
      // TODO: Instead of forcibly losing data, prompt for retry or abort.
      new_config['events'] = structuredClone(cur_config['events']);
    }).always(function () {
      setTimeout(config_update_sync, 0, failure_cnt);
    });
    return;
  }

  console.log("Config sync complete");
  $("#config-form").removeClass("syncing");
  $("#btn-save").prop('disabled', false);
  if (failure_cnt > 0) refresh_config_ui();
}

function config_update_probe() {
  if (config_update_timer) {
    clearTimeout(config_update_timer);
    config_update_timer = null;
  }

  if (_.isEqual(new_config, cur_config)) {
    $("#btn-save").prop('disabled', false);
    return;
  }

  $("#btn-save").prop('disabled', true);
  config_update_timer = setTimeout(function () {
    config_update_timer = null;
    $("#config-form").addClass("syncing");
    console.log("Config sync start");
    config_update_sync(0);
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

function print_transition_params(transition, abbrv) {
  if (!('duration_ms' in transition)) return "(unspecified)";

  var text = '';

  if (!abbrv) text += `Duration: `;
  text += (transition['duration_ms'] / 1000).toFixed(1) + 's';

  if ('color' in transition) {
    text += abbrv ? ', ' : '<br>Color: ';
    const bg_color = transition['color'];
    const fg_color = get_fg_color(bg_color);
    text += `<span style="color:${fg_color};background-color:${bg_color}" class="color-sample">${bg_color}</span>`;
  }

  if (transition['type'] == 'color-wipe') {
    const dir = transition['direction'];
    text += abbrv ? ', ' : '<br>Direction: ';
    switch (dir) {
      case 'LtR': text += 'Start->End'; break;
      case 'RtL': text += 'Start<-End'; break;
      default: text += `(unrecognized)`;
    }

    if (!abbrv) {
      text += `<br>Blade width: ${(transition['blade_width'] * 100).toFixed(1)}%`;
    }
  }
  return text;
}

function config_received(config) {
  new_config = structuredClone(cur_config = config || {});
  if (!new_config.transitions) {
    cur_config['transitions'] = {};
    new_config['transitions'] = {};
  }
  if (!new_config.events) {
    cur_config['events'] = {};
    new_config['events'] = {};
  }
  refresh_config_ui();
}

function refresh_config_ui() {
  // Populate widgets from config
  if (new_config.num_pixels) {
    var strip_length_slider = $("#strip-length-slider");
    strip_length_slider.slider("value", new_config.num_pixels);
    strip_length_slider.find("input").val(new_config.num_pixels);
  }

  const transition_list = $("#strip-transition-list");
  transition_list.find("tr:not(#new-transition)").remove();
  const new_transition_row = transition_list.find("#new-transition");
  for (var name in new_config.transitions) {
    const transition_row = create_transition_row(name, new_config.transitions[name]);
    new_transition_row.before(transition_row);
  }

  const event_list = $("#event-list");
  event_list.find("li:not(.list-append)").remove();
  for (var idx in new_config.events) {
    create_event_entry(new_config.events[idx], event_list);
  }
}

function enter_setup() {
  probe_url_for(URL_TWILIGHT_SETUP + '?' + $.param({ "state": "enter" }),
    function () { $("#config-form").removeClass("syncing"); },
    function (text) {
      notify_prompt(`<p>Enter setup mode failed:<br>${text}
        <p>Click OK to try again.`, enter_setup, {
        esc_close: false, backdrop_close: false
      });
  });
}

function exit_setup_discard() {
  if (config_update_timer) {
    clearTimeout(config_update_timer);
    config_update_timer = null;
    $("#btn-save").prop('disabled', false);
  }

  if (DEVMODE) return config_received(structuredClone(DEBUG_CONFIG));

  $("#config-form").addClass("syncing");
  probe_url_for(URL_TWILIGHT_SETUP + '?' + $.param({ "state": "exit-discard" }),
    function () {
      block_prompt("<p>Set up discarded.<p>Reloading configuration...");
      probe_url_for(URL_TWILIGHT_SETUP, function (payload) {
        config_received(payload['config']);
        notify_prompt("<p>Click OK to enter setup mode...", enter_setup, {
          esc_close: false, backdrop_close: false
        });
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
        <p>Click OK to enter setup mode...`, enter_setup, {
        esc_close: false, backdrop_close: false
      });
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

      // Update all references
      var update_cnt = 0;
      const event_list = $("#event-list");
      event_list.find("li:not(.list-append)").each(function (idx, elem) {
        var event_list_entry = $(elem);
        const event = event_list_entry.data('event');
        var updated = false;
        for (var trans_idx in event['transitions']) {
          if (event['transitions'][trans_idx] == old_name) {
            event['transitions'][trans_idx] = new_name;
            update_cnt++; updated = true;
          }
        }
        // If updated, need to refresh UI.
        if (updated) update_event_entry(event, event_list_entry);
      });
      if (update_cnt > 0) {
        console.log(`Updated ${update_cnt} references`);
        event_list_update();
      }
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
  test_button.prop('disabled', true);

  const transition = dialog.data('collect-params')();
  console.log("Testing transition parameters:", transition);

  $.ajax({
    method: 'POST',
    url: URL_TWILIGHT_SETUP + '?' + $.param({ [PARAM_TEST_TRANSITION]: null }),
    data: JSON.stringify(transition),
    contentType: 'application/json',
  }).done(function () {
    console.log(`Sent transition params for testing`);
  }).fail(function (jqXHR, textStatus) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : textStatus;
    console.log("Failed to test transition params:", resp_text);
  }).always(function () {
    test_button.prop('disabled', false);
  });
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
    var new_params = structuredClone(transition);
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
    esc_close: true, backdrop_close: false
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

function transition_remove_check(name) {
  // Check if the transition is referred
  var event_cnt = 0;
  for (var idx in new_config['events']) {
    const event = new_config['events'][idx];
    if (event['transitions'].includes(name))
      event_cnt++;
  }
  if (event_cnt > 0) {
    notify_prompt(`<p>Cannot remove "${name}".<p>There are ${event_cnt} events referring to it.`);
    return;
  } else {
    confirm_prompt(`<p>Remove transition "${name}"?`, remove_transition, { data: name });
  }
}

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
    switch (evt.which) {
      case 27:
        // Escape leaves editing mode without saving
        discard_exit_edit();
        break;

      case 13:
        evt.preventDefault();
        // Save and leave editing
        save_exit_edit(this.value);
        break;

      case 9:
        evt.preventDefault();
        // Save and leave editing
        if (save_exit_edit(this.value)) {
          // Tab moves editing to the next cell
          transition_edit_type(row, cell.next());
        } 
        break;
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

    // Removing leading space from `norm_selection_text` if `norm_before_selection_text` already ends with a space
    if (norm_before_selection_text.endsWith(' ') && norm_selection_text.startsWith(' ')) {
      norm_selection_text = norm_selection_text.slice(1);
    }

    if (norm_selection_text.length == 0) {
      // Removing leading space from `norm_after_selection_text` if `norm_before_selection_text` already ends with a space
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
  var cur_option = null;
  for (var type in TRANSITIONS_SETUP) {
    var option = $(`<option></option>`);
    option.text(type);
    if (transition['type'] == type) cur_option = option;
    select.append(option);
  }
  cell.append(select);
  if (cur_option) {
    cur_option.prop('selected', true);
    cur_option.css('font-weight', 'bolder');
    cur_option.css('text-decoration', 'underline');
    cur_option[0].scrollIntoView();
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
    switch (evt.which) {
      case 27:
        // Restore the current transition display
        const selected = select.find('option:selected').val();
        if (selected != transition['type'] && transition['type']) {
          row.find('.transition-params').html(print_transition_params(transition));
        }
        remove_select();
        break;

      case 13:
        evt.preventDefault();
        // Confirm and save transition type
        confirm_transition_type_change(select, function () {
          // Exit editing
          remove_select();
        });
        break;

      case 9:
        evt.preventDefault();
        // Confirm and save transition type
        confirm_transition_type_change(select, function () {
          // Moves editing to other cells
          remove_select();
          if (evt.shiftKey) transition_edit_name(row, cell.prev());
          else transition_edit_params(row, cell.next());
        });
        break;
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

function create_transition_row(name, data) {
  const transition_row = $(`<tr>
        <td class="transition-name">${name || 'new-transition'}</td>
        <td class="transition-type">${data['type'] || '(to be specified)'}</td>
        <td class="transition-params">${data ? print_transition_params(data) : '(to be specified)'}</td>
        <td class="list-remove"></td>
      </tr>`);
  transition_row.data('name', name);
  transition_row.data('transition', data);
  return transition_row;
}

function click_transition_cell(evt) {
  var cell = $(evt.target);
  const row = cell.parent();

  if (cell.hasClass('list-append')) {
    console.log("Clicked on append row");

    const new_row = create_transition_row('', {});
    row.before(new_row);
    transition_edit_name(new_row, new_row.find(".transition-name"));
  } else {
    const row_name = row.data('name');
    console.log(`Clicked on transition row: '${row_name}'`);

    if (cell.hasClass('list-remove')) {
      transition_remove_check(row_name);
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

function setup_cc_input(input, picker, ctype, ccstr, options) {
  input_setup_numeric(input,
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

  input_setup_numeric(input_rgb, {
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

function init_weekly_checkboxes(container) {
  const weekday_list = get_locale_weekday_list();
  container.empty();
  for (var idx in weekday_list) {
    const weekday = weekday_list[idx];
    const checkbox = $(`<label for="event-params:recurrence-weekly:${weekday[0]}">
      <input type="checkbox" id="event-params:recurrence-weekly:${weekday[0]}" value="${weekday[0]}">
      <span>${weekday[1]}</span>
    </label>`);
    container.append(checkbox);
  }
}

function init_annual_month_select(container) {
  const month_list = get_locale_month_list(undefined, "long");
  container.empty();
  for (var idx in month_list) {
    const option = $(`<option value="${idx}">${month_list[idx]}</option>`);
    container.append(option);
  }
  container.val(undefined);
  enable_select_wheeling(container);
}

function init_annual_day_select(container) {
  for (var idx = 1; idx <= 31; idx++) {
    const option = $(`<option value="${idx}">&nbsp;${idx}&nbsp;</option>`);
    if (idx >= 30) option.prop('disabled', true);
    container.append(option);
  }
  container.val(undefined);
  enable_select_wheeling(container);
}

function event_month_update() {
  const month_idx = Number($("#event-params\\:recurrence-annual\\:month").val());
  const event_day_select = $("#event-params\\:recurrence-annual\\:day");
  const event_day = Number(event_day_select.val());
  if ([1, 3, 5, 7, 8, 10, 12].includes(month_idx + 1)) {
    // January, March, May, July, August, October, December enables day 30 and 31
    event_day_select.find("option[value=30]").prop('disabled', false);
    event_day_select.find("option[value=31]").prop('disabled', false);
  } else if ([4, 6, 9, 11].includes(month_idx + 1)) {
    // April, June, September, November enables day 30 and disables day 31
    if (event_day == 31) event_day_select.val(30);
    event_day_select.find("option[value=30]").prop('disabled', false);
    event_day_select.find("option[value=31]").prop('disabled', true);
  } else {
    // February and undefined disables day 30 and 31
    if ([30, 31].includes(event_day)) event_day_select.val(28);
    event_day_select.find("option[value=30]").prop('disabled', true);
    event_day_select.find("option[value=31]").prop('disabled', true);
  }
}

function recurrence_type_change() {
  switch (this.id) {
    case "event-params:recurrence-type:daily":
      $("#event-params\\:recurrence-weekly").addClass("hidden");
      $("#event-params\\:recurrence-annual").addClass("hidden");
      break;
    case "event-params:recurrence-type:weekly":
      $("#event-params\\:recurrence-weekly").removeClass("hidden");
      $("#event-params\\:recurrence-annual").addClass("hidden");
      break;
    case "event-params:recurrence-type:annual":
      $("#event-params\\:recurrence-weekly").addClass("hidden");
      $("#event-params\\:recurrence-annual").removeClass("hidden");
      break;
  }
}

function recurrent_event_presubmit() {
  const recurrence_type_checked = this.find("input[name='event-params\\:recurrence-type']:checked");
  recurrence_type = recurrence_type_checked[0].id.split(':')[2];
  switch (recurrence_type) {
    case "weekly":
      const weekly_checkboxes = this.find("input[id^='event-params\\:recurrence-weekly\\:']:checked");
      if (weekly_checkboxes.length == 0) {
        notify_prompt("No weekday selected!");
        return false;
      }
      break;

    case "annual":
      const month_selected = this.find("#event-params\\:recurrence-annual\\:month>:selected");
      if (month_selected.length == 0) {
        notify_prompt("No month selected!");
        return false;
      }
      const day_selected = this.find("#event-params\\:recurrence-annual\\:day>:selected");
      if (day_selected.length == 0) {
        notify_prompt("No day of month selected!");
        return false;
      }
      break;

    default:
      console.assert(recurrence_type == "daily", "Unknown recurrence type: " + recurrence_type);
  }

  const transition_entries = this.find("#event-params\\:recurrent\\:transition-list>li:not(.list-append)");
  if (transition_entries.length == 0) {
    notify_prompt("No transition specified!");
    return false;
  }

  return true;
}

function recurrent_event_collect_data() {
  var result = {};

  const time_start = this.find("#event-params\\:recurrence-daily\\:start").data("TDEx").getTime()[0];
  result['daily'] = [Math.floor(time_start / 60).toFixed(0)];
  const time_end_enable = this.find("#event-params\\:recurrence-daily\\:end-enable");
  if (time_end_enable.is(":checked")) {
    const time_end = this.find("#event-params\\:recurrence-daily\\:end").data("TDEx").getTime()[0];
    result['daily'].push(Math.floor(time_end / 60).toFixed(0));
  }

  const recurrence_type_checked = this.find("input[name='event-params\\:recurrence-type']:checked");
  recurrence_type = recurrence_type_checked[0].id.split(':')[2];
  switch (recurrence_type) {
    case "weekly":
      var weekly_selected = [];
      const weekly_checkboxes = this.find("input[id^='event-params\\:recurrence-weekly\\:']:checked");
      weekly_checkboxes.each(function (idx, elem) {
        weekly_selected.push(elem.value);
      });
      result['weekly'] = weekly_selected;
      break;

    case "annual":
      const month_selected = this.find("#event-params\\:recurrence-annual\\:month>:selected");
      const day_selected = this.find("#event-params\\:recurrence-annual\\:day>:selected");
      result['annual'] = [month_selected.val(), day_selected.val()];
      break;

    default:
      console.assert(recurrence_type == "daily", "Unknown recurrence type: " + recurrence_type);
  }
  result['type'] = recurrence_type;

  var transition_names = [];
  const transition_entries = this.find("#event-params\\:recurrent\\:transition-list>li:not(.list-append)");
  transition_entries.each(function (idx, elem) {
    transition_names.push($(elem).data('name'));
  });
  result['transitions'] = transition_names;

  console.log("Recurrent event data:", result);
  return result;
}

function print_event_desc(event) {
  var result = "";
  switch (event.type) {
    case "daily": result += "Daily "; break;

    case "weekly":
      const weekday_list = get_locale_weekday_list();
      var print_weekdays = [];
      for (var idx in weekday_list) {
        const weekday = weekday_list[idx];
        if (event.weekly.includes(weekday[0].toFixed(0))) {
          print_weekdays.push(weekday[1]);
        }
      }
      result += `Every (${print_weekdays.join(', ')}) `;
      break;

    case "annual":
      const month_list = get_locale_month_list(undefined, "long");
      result += `On ${month_list[Number(event.annual[0])]} ${event.annual[1]} `;
      break;

    default:
      return "<unknown event type>";
  }

  const time_formatter = new Intl.DateTimeFormat(undefined, {
    timeZone: "UTC", hour: 'numeric', minute: '2-digit'
  });
  if (event.daily.length == 1) {
    const start_time = new Date(event.daily[0] * 60 * 1000);
    result += `at ${time_formatter.format(start_time)}`;
  } else {
    const start_time = new Date(event.daily[0] * 60 * 1000);
    const end_time = new Date(event.daily[1] * 60 * 1000);
    result += `${time_formatter.format(start_time)}~${time_formatter.format(end_time)}`;
  }

  result += ";<br>Transition(s): " + event.transitions.join(', ');
  return result;
}

function event_list_update() {
  const event_list = $("#event-list");
  var events = [];
  event_list.find("li:not(.list-append)").each(function (idx, elem) {
    events.push($(elem).data('event'));
  });
  new_config.events = events;
  config_update_probe();
}

function remove_event(evt) {
  evt.stopPropagation();
  const event_entry = $(evt.currentTarget).parent();
  const event = event_entry.data('event');
  confirm_prompt(`<p>Confirm removing event:
    <p>${print_event_desc(event)}`, function () {
    event_entry.remove();
    event_list_update();
  }, {
    esc_close: true, backdrop_close: true
  });
}

function edit_event(evt) {
  const event_entry = $(evt.currentTarget);
  const event = event_entry.data('event');
  prompt_recurrent_event_config(event, function (event_data) {
    if (_.isEqual(event, event_data)) {
      console.log("Event parameters unchanged");
      return;
    }

    update_event_entry(event_data, event_entry);
    event_list_update();
  });
}

function update_event_entry(event_data, event_entry) {
  event_entry.data('event', event_data);
  event_entry.find('span.event-desc').html(print_event_desc(event_data));
}

function create_event_entry(event_data, event_list) {
  const event_list_append = event_list.find('.list-append');
  const event_row = $(`<li>
    <span class="event-desc"></span>
    <span class="list-remove"></span>
  </li>`);
  update_event_entry(event_data, event_row);
  event_list_append.before(event_row);
}

function append_new_event() {
  prompt_recurrent_event_config({}, function (event_data) {
    create_event_entry(event_data, $("#event-list"));
    event_list_update();
  });
}

function prompt_recurrent_event_config(event, save_action) {
  const dialog = $("dialog#event-params\\:recurrent");
  dialog.find(`input[id^='event-params\\:recurrence-weekly\\:']`).prop('checked', false);
  dialog.find(`select[id^='event-params\\:recurrence-annual\\:']`).val(undefined);
  const tdex_start = dialog.find('#event-params\\:recurrence-daily\\:start').data('TDEx');
  const tdex_end = dialog.find('#event-params\\:recurrence-daily\\:end').data('TDEx');
  const tdex_end_enable = dialog.find('#event-params\\:recurrence-daily\\:end-enable');

  if (event.type) {
    switch (event.type) {
      case "weekly":
        for (var idx in event.weekly) {
          const weekday_num = event.weekly[idx];
          dialog.find(`#event-params\\:recurrence-weekly\\:${weekday_num}`).prop('checked', true);
        }
        break;

      case "annual":
        dialog.find('#event-params\\:recurrence-annual\\:month').val(event.annual[0]);
        dialog.find('#event-params\\:recurrence-annual\\:day').val(event.annual[1]);
        break;

      default:
        console.assert(event.type == "daily", "Unknown recurrence type: " + event.type);
    }
    dialog.find(`#event-params\\:recurrence-type\\:${event.type}`).prop('checked', true).trigger('change');

    tdex_start.setTime(event.daily[0] * 60);
    tdex_start.select('hr');
    if (event.daily.length > 1) {
      tdex_end.setTime(event.daily[1] * 60);
      tdex_end_enable.prop('checked', true).trigger('change');
    } else {
      tdex_end_enable.prop('checked', false).trigger('change');
    }
  } else {
    dialog.find('#event-params\\:recurrence-type\\:daily').prop('checked', true).trigger('change');
    tdex_start.setTime(0);
    tdex_start.select('hr');
    tdex_end_enable.prop('checked', false).trigger('change');
  }

  const transition_list = dialog.find("#event-params\\:recurrent\\:transition-list");
  transition_list.find("li:not(.list-append)").remove();
  const transition_list_append = transition_list.find('.list-append');
  for (var idx in event.transitions) {
    const transition_name = event.transitions[idx];
    const transition_entry = $(`<li></li>`);
    update_transition_entry(transition_entry, transition_name);
    transition_list_append.before(transition_entry);
  }

  dialog_confirm_prompt(dialog, undefined, function () {
    const event_data = recurrent_event_collect_data.apply(this);
    save_action(event_data);
  }, {
    esc_close: true, backdrop_close: false,
    presubmit: recurrent_event_presubmit
  });
}

function update_transition_entry(transition_entry, transition_name) {
  const transition = new_config.transitions[transition_name];
  transition_entry.data('name', transition_name);
  transition_entry.html(`<span class="item-name">${transition_name}</span> |
      <span class="item-desc">
        ${transition.type + ': ' + print_transition_params(transition, true)}
      </span><span class="list-remove"></span>`);
}

function event_time_end_enable_change() {
  const event_time_end = $("#event-params\\:recurrence-daily\\:end");
  const tdex = event_time_end.data('TDEx');
  if (this.checked) tdex.select('hr');
  else {
    tdex.setTime(24 * 3600);
    tdex.select(undefined);
  }
}

function append_new_transition(evt) {
  const new_transition = $(`<li></li>`);
  $(evt.target).before(new_transition);
  transition_select(new_transition);
}

function transition_select(list_entry) {
  const cur_transition_name = list_entry.data('name');
  const select = $(`<select></select>`);
  var cur_option = null;
  for (var name in new_config.transitions) {
    const option = $(`<option value="${name}">${name}</option>`);
    if (cur_transition_name == name) cur_option = option;
    select.append(option);
  }
  enable_select_wheeling(select);
  if (cur_option) {
    cur_option.css('font-weight', 'bolder');
    cur_option.css('text-decoration', 'underline');
  }
  select.val(cur_transition_name);
  list_entry.empty();
  list_entry.append(select);
  list_entry.addClass("item-edit");

  select.focus();
  select.on('keydown', function (evt) {
    switch (evt.which) {
      case 27:
        select.val(cur_transition_name);
        dialog_element_unfocus($(this));
        break;

      case 13:
        transition_finalize.apply(this);
    }
  });
  select.on('blur', transition_finalize);
}

function transition_finalize() {
  const select = $(this);
  const transition_entry = select.parent();
  const transition_name = select.val();
  select.remove();

  if (transition_name) {
    update_transition_entry(transition_entry, transition_name);
    transition_entry.removeClass("item-edit");
  } else {
    transition_entry.remove();
  }
}

function transition_reselect(evt) {
  const list_entry = $(evt.currentTarget);
  transition_select(list_entry);
}

$(function () {
  block_prompt("<p><center>Fetching configuration...</center>");

  //--------------------
  // Initialize setup page elements

  // -- Strip config
  input_setup_num_slider($("#strip-length-slider"), {
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

  // -- NTP & Timezone config
  $('#redir-ntp-tz').on('click', function () {
    window.top.location.href = "index.html#sys-mgmt";
  });

  // -- Events config
  const event_list = $("#event-list");
  event_list.sortable({
    opacity: 0.8,
    placeholder: "placeholder",
    items: "li:not(.list-append)",
    stop: event_list_update
  });
  event_list.find(".list-append").on("click", append_new_event);
  event_list.on("dblclick", "li:not(.list-append)", edit_event);
  event_list.on("dblclick", ".list-remove", remove_event);

  // -- Transition params config dialogs
  input_setup_num_slider($("#uniform-color\\:duration-slider"), {
    min: 0, max: 60, step: 0.1, pagecount: 60,
    default: 3, fixedpoint: 1
  });
  setup_color_picker("#uniform-color\\:color-picker");

  // -- Event params config dialogs
  $("input[id^='event-params\\:recurrence-type']").on('change', recurrence_type_change);
  init_weekly_checkboxes($("#event-params\\:recurrence-weekly"));
  const event_month_select = $("#event-params\\:recurrence-annual\\:month");
  init_annual_month_select(event_month_select);
  event_month_select.on('change', event_month_update);
  const event_day_select = $("#event-params\\:recurrence-annual\\:day");
  init_annual_day_select(event_day_select);

  const event_time_start = $("#event-params\\:recurrence-daily\\:start");
  const event_time_start_ctrl = event_time_start.timeDropper({
    inline: true, alwaysOpen: true, alwaysDial: true, autoSwitch: 1,
    range: { start: 0, length: 24 * 3600, init: 0 },
    container: event_time_start.siblings(".container"),
  });
  const event_time_end = $("#event-params\\:recurrence-daily\\:end");
  const event_time_end_ctrl = event_time_end.timeDropper({
    inline: true, alwaysOpen: true, alwaysDial: true, autoSwitch: 1,
    range: { start: 0, length: 24 * 3600, init: 24 * 3600 },
    container: event_time_end.siblings(".container"),
  });
  event_time_start_ctrl.on("TDEx-update", function (evt, data) {
    const start_time = data.time[0];
    event_time_end.data('TDEx').updateBound(start_time, 24 * 3600);
  });
  event_time_end_ctrl.on("TDEx-update", function (evt, data) {
    const end_time = data.time[0];
    event_time_start.data('TDEx').updateBound(0, end_time);
  });
  const event_time_end_enable = $("#event-params\\:recurrence-daily\\:end-enable");
  event_time_end_enable.on('change', event_time_end_enable_change);

  const transition_lists = $("ol[id$='\:transition-list']");
  transition_lists.sortable({
    opacity: 0.8,
    placeholder: "placeholder",
    items: "li:not(.list-append, .item-edit)",
    cancel: "li.list-append, li.item-edit",
  });
  transition_lists.each(function (idx, elem) {
    const list = $(elem);
    list.find(".list-append").on("click", append_new_transition);
    list.on("dblclick", "li:not(.list-append, .item-edit)", transition_reselect);
    list.on("dblclick", ".list-remove", function (evt) {
      evt.stopPropagation();
      $(evt.target).parent().remove();
    });
  });

  //...

  // Hook the test buttons for all dialogs
  $("dialog.config").each(function (idx, elem) {
    const dialog = $(elem);
    dialog.find(".dlg-ctrl>input.test-button").click(function () {
      transition_params_test(dialog);
    });
  });

  //--------------------
  // Page control
  $("#btn-discard").click(function () {
    confirm_prompt("<p>Discard changes and exit setup mode?", exit_setup_discard, {
      esc_close: true, backdrop_close: true
    });
  });
  $("#config-form").submit(function (evt) {
    evt.preventDefault();
    confirm_prompt("<p>Save changes and exit setup mode?", exit_setup_save, {
      esc_close: true, backdrop_close: true
    });
  });

  if (!DEVMODE) {
    probe_url_for(URL_TWILIGHT_SETUP, function (payload) {
      config_received(payload['config']);
      if (payload['setup']) {
        prompt_close();
        $("#config-form").removeClass("syncing");
      } else {
        notify_prompt("<p>Click OK to enter setup mode...", enter_setup, {
          esc_close: false, backdrop_close: false
        });
      }
    }, function (text) {
      block_prompt(`<p><center>TWiLight unavailable:<br>${text}</center>`);
    });
  } else {
    setTimeout(function () {
      prompt_close();
      setTimeout(function () {
        $("#config-form").removeClass("syncing");
        config_received(structuredClone(DEBUG_CONFIG));
      }, 500);
    }, 200);
  }
});