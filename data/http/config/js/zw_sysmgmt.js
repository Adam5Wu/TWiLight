var DEVMODE = window.location.protocol === 'file:';
var BOOT_SERIAL;

//-----------------------
// Reboot
const URL_REBOOT = "reboot.html";

function reboot_click() {
  if (!BOOT_SERIAL) return;
  confirm_prompt("<p>Confirm reboot?", do_reboot);
}

function do_reboot() {
  window.location.href = URL_REBOOT;
}

//-----------------------
// Storage Backup
const URL_STORAGE = "/!sys/storage";

function storage_backup_click() {
  var download_link = document.createElement('a');
  download_link.href = URL_STORAGE;
  download_link.setAttribute('download', '');
  download_link.target = 'download-frame';
  download_link.click();
}

//-----------------------
// Storage Restore

function storage_restore_click(evt) {
  if (!BOOT_SERIAL) return;
  if (!$(evt.target).is('#storage-file-label')) {
    $('#storage-file-label').trigger('click');
  }
}

function drop_storage_file(evt) {
  if (!BOOT_SERIAL) return;
  evt.preventDefault();
  $(this).removeClass('drag-hover');

  var storage_file = $("#storage-file")[0];
  storage_file.files = evt.originalEvent.dataTransfer.files;
  $(storage_file).trigger('change');
}

function storage_file_select() {
  const file = this.files[0];
  if (file) {
    console.log("Selected file:", file);

    if (STORAGE_UPLOADING) {
      console.log("Ignored storage file change due to upload in progress.");
    }

    const reader = new FileReader();
    reader.onload = process_storage_data;
    reader.readAsArrayBuffer(file);
  }
}

var STORAGE_UPLOADING = false;

function bad_storage_data_message(detail) {
  return `<p>Not a LittleFS image or corrupted data${detail ? ":<p>" + detail : "."}`;
}

function process_storage_data(evt) {
  const storage_data = evt.target.result;

  const decoder = new TextDecoder('utf-8');
  const image_magic_view = new Uint8Array(storage_data, 8, 8);
  const image_magic = decoder.decode(image_magic_view);
  console.log("Image file magic: ", image_magic);

  if (image_magic != "littlefs") {
    return notify_prompt(bad_storage_data_message("Invalid image magic"));
  }

  confirm_prompt("<p>This will replacing all user settings.<p>Proceed?",
    send_storage_data, { data: storage_data });
}

function send_storage_progress(evt) {
  if (evt.lengthComputable) {
    var percentComplete = evt.loaded * 100 / evt.total;
    console.log("Upload progress: " + Number.parseFloat(percentComplete).toFixed(2) + '%');

    const dark_scheme = window.matchMedia("(prefers-color-scheme: dark)");
    const prog_lower = Number.parseFloat(percentComplete - 2).toFixed(1);
    const prog_higher = Number.parseFloat(percentComplete + 3).toFixed(1);
    $("#storage-restore").css("background-image",
      `linear-gradient(0deg, ${dark_scheme ? "teal" : "lightblue"} ${prog_lower}%, transparent ${prog_higher}%)`);
  }
}

function send_storage_data(storage_data) {
  STORAGE_UPLOADING = true;
  $.ajax({
    method: "PUT",
    url: URL_STORAGE + '?' + $.param({ "bs": BOOT_SERIAL }),
    data: storage_data,
    processData: false,
    contentType: 'application/octet-stream',
    xhr: function () {
      var xhr = new window.XMLHttpRequest();
      xhr.upload.addEventListener("progress", send_storage_progress, false);
      return xhr;
    }
  }).done(function () {
    $("#storage-restore").css("background-image", "");
    confirm_prompt("<p>Upload complete.<p>A reboot is highly recommended, proceed?", do_reboot);
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    notify_prompt(`<p>Upload failed${resp_text ? ": " + resp_text : "."}`);
  }).always(function () {
    STORAGE_UPLOADING = false;
  });
}

//-----------------------
// Storage Reset

function storage_reset_click(evt) {
  if (!BOOT_SERIAL) return;
  confirm_prompt("<p>This will erase all user settings and reboot.<p>Proceed?", do_storage_reset);
}

function do_storage_reset() {
  $.ajax({
    method: "DELETE",
    url: URL_STORAGE + '?' + $.param({ "bs": BOOT_SERIAL }),
  }).done(function () {
    block_prompt("<p>User setting erased. Rebooting in 3 seconds...");
    setTimeout(function () { do_reboot(); }, 3000);
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    notify_prompt(`<p>Setting reset failed${resp_text ? ": " + resp_text : "."}`);
  });
}

//-----------------------
// Time Setup
const URL_CONFIG = "/!sys/config";
const CONFIG_SECTION_TIME = "time";
const CONFIG_TIME_DEFAULT_NTP_SERVER = "pool.ntp.org";

function parse_tz_offset(offset_str) {
  const ofs_match = offset_str.match(/GMT([+-]\d{1,2}:\d{2})/);
  return ofs_match ? parse_time_offset(ofs_match[1]) : 0;
}

function resolve_tz_info(formatters, datetime) {
  const tz_locale = formatters.locale.format(datetime).split(' ').slice(1).join(" ");
  const tz_canonical = formatters.canonical.format(datetime).split(' ').slice(1).join(" ");
  const tz_abbrv = formatters.abbrv.format(datetime).split(' ').slice(1).join(" ");
  const tz_ofs = formatters.offset.format(datetime).split(' ', 2)[1];

  // This timezone has no semantic name, ignore
  if (tz_canonical == tz_ofs) return null;

  return {
    id: tz_canonical, name: tz_locale,
    abbrv: tz_abbrv.startsWith("GMT") ? null : tz_abbrv,
    offset: [tz_ofs, parse_tz_offset(tz_ofs)],
  };
}

function probe_offset_change(date1, date2, offset, formatter) {
  var time_diff = Math.abs(date2.getTime() - date1.getTime());
  const one_minute = 1000 * 60;
  while (time_diff > one_minute) {
    const mid_date = new Date(date2.getTime() - time_diff / 2);
    const new_offset = formatter.format(mid_date).split(' ', 2)[1];

    if (new_offset != offset) date2 = mid_date;
    else date1 = mid_date;
    time_diff = Math.abs(date2.getTime() - date1.getTime());
  }
  return new Date(Math.floor(date2.getTime() / one_minute) * one_minute);
}

function prep_time_change_info(date, adjust, formatter, weekday_map) {
  const pre_date = new Date(date.getTime() - 1);
  const pre_date_parts = formatter.formatToParts(pre_date);
  var month_idx = Math.floor(pre_date_parts.find(part => part.type == 'month').value) - 1;
  var week_idx = Math.floor(Number(pre_date_parts.find(part => part.type == 'day').value) / 7);
  var dow = weekday_map[pre_date_parts.find(part => part.type == 'weekday').value];
  var at_min = Number(pre_date_parts.find(part => part.type == 'minute').value) + 1;

  var at_hour = Number(pre_date_parts.find(part => part.type == 'hour').value);
  if (at_min == 60) { at_hour += 1; at_min = 0; }

  if (at_hour == 24 && adjust > 0) {
    const at_date_parts = formatter.formatToParts(date);
    month_idx = Math.floor(at_date_parts.find(part => part.type == 'month').value) - 1;
    week_idx = Math.floor(Number(at_date_parts.find(part => part.type == 'day').value) / 7);
    dow = weekday_map[at_date_parts.find(part => part.type == 'weekday').value];
    at_hour = 0;
  }

  // If it happens beyond the third week, just treat it as the last week
  if (week_idx > 2) week_idx = -1;
  return {
    month_idx: month_idx, week_idx: week_idx, dow: dow,
    trigger: at_hour * 60 + at_min, adjust: adjust
  };
}

function print_time_offset(minutes, short = {}) {
  var result = minutes >= 0 ? (short.no_plus ? '' : '+') : '-';
  const abs_minutes = Math.abs(minutes);
  const hour_str = Math.floor(abs_minutes / 60).toString();
  result += short.whole_hr ? hour_str : hour_str.padStart(2, '0');
  const mins = abs_minutes % 60;
  if (!short.whole_hr || mins) result += `:${mins.toString().padStart(2, '0')}`;
  return result;
}

function resolve_tz(timezone, utils) {
  const formatters = {
    locale: new Intl.DateTimeFormat(undefined, {
      timeZone: timezone,
      timeZoneName: "longGeneric",
    }),
    canonical: new Intl.DateTimeFormat("en-US", {
      timeZone: timezone,
      timeZoneName: "longGeneric",
    }),
    abbrv: new Intl.DateTimeFormat("en-US", {
      timeZone: timezone,
      timeZoneName: "short",
    }),
    offset: new Intl.DateTimeFormat("en-US", {
      timeZone: timezone,
      timeZoneName: "longOffset",
    })
  };
  const currentYear = new Date().getUTCFullYear();
  const local_timezone = new Intl.DateTimeFormat().resolvedOptions().timeZone == timezone;

  // Heuristically detect daylight saving
  const solstices = [
    { time: new Date(Date.UTC(currentYear - 1, 12, 21)), tz: {} },
    { time: new Date(Date.UTC(currentYear, 6, 21)), tz: {} }
  ];

  var winter = solstices[0];
  winter.tz = resolve_tz_info(formatters, winter.time);
  if (!winter.tz) return null;

  // Save time not working on the same timezone
  // Since the ID is generic it should be the same as summer time.
  if (winter.tz.id in utils.cache) {
    if (local_timezone) utils.cache[winter.tz.id]['local'] = true;
    return null;
  }

  summer = solstices[1];
  summer.tz = resolve_tz_info(formatters, summer.time);
  if (!summer.tz) return null;

  // Detect if this is southern hemisphere time
  if (winter.tz.offset[1] > summer.tz.offset[1]) {
    winter = solstices[1];
    summer = solstices[0];
  }

  var tz_info = {
    id: winter.tz.id,
    abbrv: [winter.tz.abbrv || "*LOC"],
    offsets: [winter.tz.offset[1]],
    display: `${winter.tz.offset[0].substring(3) || '&plusmn;00:00'} | ${winter.tz.name}`,
    time_formatter: new Intl.DateTimeFormat(undefined, {
      timeZone: timezone,
      hour12: true, hour: "2-digit", minute: "2-digit"
    }),
  };
  if (local_timezone) tz_info['local'] = true;

  var display_decor = winter.tz.abbrv || '';
  if (winter.tz.offset[1] != summer.tz.offset[1]) {
    tz_info.abbrv.push(summer.tz.abbrv || "*DST");
    tz_info.offsets.push(summer.tz.offset[1]);
    const dst_adjust = summer.tz.offset[1] - winter.tz.offset[1];
    const adjust_str = print_time_offset(dst_adjust, { whole_hr: true });
    if (winter.tz.abbrv) {
      if (summer.tz.abbrv) display_decor += `/${summer.tz.abbrv}`;
      else display_decor += `, *DST${adjust_str}`;
    } else display_decor = `*DST${adjust_str}`;

    // Probe switching time
    const start_time = probe_offset_change(winter.time, summer.time, winter.tz.offset[0], formatters.offset);
    const next_winter_time = new Date(new Date(winter.time).setFullYear(winter.time.getFullYear() + 1));
    const end_time = probe_offset_change(summer.time, next_winter_time, summer.tz.offset[0], formatters.offset);

    const switch_time_formatter = new Intl.DateTimeFormat(undefined, {
      timeZone: timezone, hourCycle: "h23",
      weekday: "short", month: "numeric", day: "numeric",
      hour: "numeric", minute: "numeric"
    });
    tz_info.dst_switch = [
      prep_time_change_info(start_time, dst_adjust, switch_time_formatter, utils.weekday_map),
      prep_time_change_info(end_time, -dst_adjust, switch_time_formatter, utils.weekday_map)
    ];
  }

  if (display_decor) {
    if (winter.tz.abbrv) tz_info.display += ` (${display_decor})`;
    else tz_info.display += ` [${display_decor}]`;
  }

  utils.cache[winter.tz.id] = tz_info;
  return tz_info;
}

function get_tz_string(tz_info) {
  const winter_abbrv = tz_info.abbrv[0];
  const winter_offset = tz_info.offsets[0];

  var tz_string = winter_abbrv;
  const tz_offset_str = print_time_offset(-winter_offset, { no_plus: true, whole_hr: true });
  if (tz_info.dst_switch) {
    const summer_abbrv = tz_info.abbrv[1];
    const summer_offset = tz_info.offsets[1];
    tz_string += tz_offset_str + summer_abbrv;
    const tz_adjust = summer_offset - winter_offset;
    if (tz_adjust != 60) {
      tz_string += print_time_offset(tz_adjust, { no_plus: true, whole_hr: true });
    }

    const dst_start = tz_info.dst_switch[0];
    const dst_start_weeknum = dst_start.week_idx + 1;
    tz_string += `,M${dst_start.month_idx + 1}.${dst_start_weeknum || 5}.${dst_start.dow}`;
    if (dst_start.trigger != 120) {
      tz_string += '/' + print_time_offset(dst_start.trigger, { no_plus: true, whole_hr: true });
    }
    const dst_end = tz_info.dst_switch[1];
    const dst_end_weeknum = dst_start.week_idx + 1;
    tz_string += `,M${dst_end.month_idx + 1}.${dst_end_weeknum || 5}.${dst_end.dow}`;
    if (dst_end.trigger != 120) {
      tz_string += '/' + print_time_offset(dst_end.trigger, { no_plus: true, whole_hr: true });
    }
  } else {
    if (winter_offset) tz_string += tz_offset_str;
    if (tz_string == "*LOC") tz_string = "GMT";
  }
  return tz_string;
}

function parse_time_offset(offset_str) {
  const offset_parts = offset_str.split(':', 2);
  const ofs_hr = Number(offset_parts[0]);
  const ofs_min = offset_parts[1] ? Number(offset_parts[1]) : 0;
  return ofs_hr * 60 + (ofs_hr < 0 ? -ofs_min : ofs_min);
}

function parse_time_change_desc(switch_str) {
  const switch_parts = switch_str.match(/(\d{1,2}).(\d).(\d)(?:\/(\d{1,2}(?:\:\d{2})?))?/);
  if (switch_parts) {
    const month_idx = Number(switch_parts[1]) - 1;
    if (month_idx < 0 || month_idx > 11) {
      console.log("Invalid month index:", switch_str);
      return null;
    }
    const week_num = Number(switch_parts[2]);
    if (week_num < 1 || week_num > 5) {
      console.log("Invalid week number:", switch_str);
      return null;
    }
    const week_idx = week_num == 5 ? -1 : (week_num - 1);
    const dow = Number(switch_parts[3]);
    if (dow < 0 || dow > 6) {
      console.log("Invalid day of week:", switch_str);
      return null;
    }
    const trigger = switch_parts[4] ? parse_time_offset(switch_parts[4]) : 120;
    if (trigger < 0 || trigger > 24 * 60) {
      console.log("Invalid trigger time:", switch_str);
      return null;
    }
    return {
      month_idx: month_idx, week_idx: week_idx, dow: dow,
      trigger: trigger
    }
  }
  return null;
}

function parse_tz_string(tz_string) {
  if (!tz_string || /\s/.test(tz_string)) {
    console.log("Invalid timezone string:", tz_string);
    return null;
  }
  const tz_parts = tz_string.split(',');
  if (tz_parts.length != 1 && tz_parts.length != 3) {
    console.log("Malformed timezone string:", tz_string);
    return null;
  }
  const zone_parts = tz_parts[0].split(/([+-]?\d{1,2}(:\d{2})?)/).filter(Boolean);

  const utc_offset = zone_parts[1] ? -parse_time_offset(zone_parts[1]) : 0;
  if (Math.abs(utc_offset) > 12 * 60) {
    console.log("Invalid UTC offset:", tz_string);
    return null;
  }
  var tz_info = {
    abbrv: [zone_parts[0]],
    offsets: [utc_offset],
  };
  if (zone_parts[2]) {
    if (tz_parts.length != 3) {
      console.log("Missing time change specs:", tz_string);
      return null;
    }
    tz_info.abbrv.push(zone_parts[2]);
    const tz_adjust = zone_parts[3] ? parse_time_offset(zone_parts[3]) : 60;
    if (Math.abs(tz_adjust) > 12 * 60) {
      console.log("Invalid DST adjustment:", tz_string);
      return null;
    }
    tz_info.offsets.push(tz_info.offsets[0] + tz_adjust);

    if (!tz_parts[1].startsWith('M') || !tz_parts[2].startsWith('M')) {
      console.log("Unsupported time change descriptor:", tz_string);
      return null;
    }
    tz_info.dst_switch = [
      parse_time_change_desc(tz_parts[1].substring(1)),
      parse_time_change_desc(tz_parts[2].substring(1))
    ];
    if (!tz_info.dst_switch[0] || !tz_info.dst_switch[1]) {
      console.log("Invalid time change descriptor:", tz_string);
      return null;
    }
    tz_info.dst_switch[0]['adjust'] = tz_adjust;
    tz_info.dst_switch[1]['adjust'] = -tz_adjust;
  }
  return tz_info;
}

function get_tz_list() {
  const utils = {
    cache: {},
    weekday_map: {},
  };
  const weekday_list = get_weekday_list();
  for (const idx in weekday_list) {
    const weekday = weekday_list[idx];
    utils.weekday_map[weekday[1]] = weekday[0];
  }

  var tz_list = []
  const timeZones = Intl.supportedValuesOf('timeZone');
  for (const idx in timeZones) {
    const tz_info = resolve_tz(timeZones[idx], utils);
    if (tz_info) {
      tz_info.tz_string = get_tz_string(tz_info);
      tz_list.push(tz_info);
    }
  }
  tz_list.sort((a, b) => (a.offsets[0] - b.offsets[0]) || a.display.localeCompare(b.display));
  return tz_list;
}

function get_month_list() {
  const formatter = Intl.DateTimeFormat(undefined, {
    calendar: "gregory", month: "short"
  });

  var month_list = [];
  for (var idx = 0; idx < 12; idx++) {
    const sample_time = new Date(Date.UTC(2025, idx, 15));
    const sample_text = formatter.format(sample_time);
    month_list.push(sample_text);
  }
  return month_list;
}

function get_ordinal_list() {
  // Pending support: https://github.com/tc39/ecma402/issues/494
  // const formatter = new Intl.NumberFormat(undefined, {
  //   style: 'ordinal'
  // });
  // var ordinal_list = [];
  // for (var idx = 0; idx < 3; idx++) {
  //   ordinal_list.push([idx, formatter.format(idx+1)]);
  // }
  // // Last
  // ordinal_list.push([-1, formatter.format(-1)]);
  // return ordinal_list;
  return [
    [0, '1st'], [1, '2nd'], [2, '3rd'],
    [-1, 'Last'],
  ];
}

function get_weekday_list() {
  const formatter = Intl.DateTimeFormat(undefined, {
    calendar: "gregory", weekday: "short"
  });

  var weekdays = {};
  for (var idx = 0; idx < 7; idx++) {
    const sample_time = new Date(Date.UTC(2025, 0, 10 + idx));
    const sample_text = formatter.format(sample_time);
    weekdays[sample_time.getDay()] = sample_text;
  }
  var weekday_list = [];
  const locale = formatter.resolvedOptions().locale;
  const first_dow = new Intl.Locale(locale).getWeekInfo().firstDay;
  for (var idx = 0; idx < 7; idx++) {
    const dow = (first_dow + idx) % 7;
    weekday_list.push([idx, weekdays[dow]]);
  }
  return weekday_list;
}

function get_dst_switch_time() {
  const h23_formatter = Intl.DateTimeFormat(undefined, {
    calendar: "gregory", timeZone: "UTC",
    hourCycle: "h23", hour: "2-digit", minute: "2-digit"
  });
  const h24_formatter = Intl.DateTimeFormat(undefined, {
    calendar: "gregory", timeZone: "UTC",
    hourCycle: "h24", hour: "2-digit", minute: "2-digit"
  });

  var time_list = [];
  const one_hour = 60 * 60 * 1000;
  for (var idx = 0; idx < 6; idx++) {
    for (var midx = 0; midx <= 45; midx += 15) {
      const sample_time = new Date(Date.UTC(2025, 0, 15, idx, midx) - 2 * one_hour);
      const sample_h24_text = h24_formatter.format(sample_time);
      const sample_h23_text = h23_formatter.format(sample_time);
      const sample_minute = sample_time.getUTCHours() * 60 + sample_time.getUTCMinutes();
      if (sample_minute) {
        const time_str = sample_minute >= 12 * 60 ? sample_h24_text : sample_h23_text;
        time_list.push([sample_minute, time_str]);
      }
      else {
        time_list.push([24 * 60, sample_h24_text]);
        time_list.push([0, sample_h23_text]);
      }
    }
  }
  return time_list;
}

function handle_alt_switch_time(select, trigger) {
  const hour = Math.floor(trigger / 60);
  const minute = trigger % 60;
  const option_text = `${hour.toString().padStart(2, '0')}:${minute.toString().padStart(2, '0')}`;
  const custom_option = $(`<option value="${trigger}" disabled>${option_text}</option>`);

  const noon_time = 12 * 60;
  var inserted = false;
  select.find("option").each(function (idx, item) {
    const item_trigger = Number(item.value);
    if (item_trigger > trigger) {
      // Ensure inserting at the right "half" of the list.
      if (!((trigger > noon_time) ^ (item_trigger > noon_time))) {
        $(item).before(custom_option);
        inserted = true;
        return false;
      }
    }
  });
  if (!inserted) select.append(custom_option);
  select.val(trigger);
}

function timezone_control_update(tz_info) {
  const TZutcOffset = $("#config-timezone-utc-offset");
  const TZdstAdjust = $("#config-timezone-dst-adjust");
  const TZmonthStart = $("#config-timezone-dst-start-month");
  const TZweekNumStart = $("#config-timezone-dst-start-weeknum");
  const TZweekdayStart = $("#config-timezone-dst-start-dow");
  const TZdstSwitchTimeStart = $("#config-timezone-dst-start-time");
  const TZmonthEnd = $("#config-timezone-dst-end-month");
  const TZweekNumEnd = $("#config-timezone-dst-end-weeknum");
  const TZweekdayEnd = $("#config-timezone-dst-end-dow");
  const TZdstSwitchTimeEnd = $("#config-timezone-dst-end-time");

  TZutcOffset.val(tz_info ? print_time_offset(tz_info.offsets[0]) : undefined);
  if (tz_info && tz_info.dst_switch) {
    TZdstAdjust.val(print_time_offset(tz_info.dst_switch[0].adjust));
    TZmonthStart.val(tz_info.dst_switch[0].month_idx);
    TZweekNumStart.val(tz_info.dst_switch[0].week_idx);
    TZweekdayStart.val(tz_info.dst_switch[0].dow);
    TZdstSwitchTimeStart.val(tz_info.dst_switch[0].trigger);
    if (TZdstSwitchTimeStart.find(":selected").length == 0)
      handle_alt_switch_time(TZdstSwitchTimeStart, tz_info.dst_switch[0].trigger);
    TZmonthEnd.val(tz_info.dst_switch[1].month_idx);
    TZweekNumEnd.val(tz_info.dst_switch[1].week_idx);
    TZweekdayEnd.val(tz_info.dst_switch[1].dow);
    TZdstSwitchTimeEnd.val(tz_info.dst_switch[1].trigger);
    if (TZdstSwitchTimeEnd.find(":selected").length == 0)
      handle_alt_switch_time(TZdstSwitchTimeEnd, tz_info.dst_switch[1].trigger);
  } else {
    TZdstAdjust.val(print_time_offset(0));
    TZmonthStart.val(undefined);
    TZweekNumStart.val(undefined);
    TZweekdayStart.val(undefined);
    TZdstSwitchTimeStart.val(undefined);
    TZmonthEnd.val(undefined);
    TZweekNumEnd.val(undefined);
    TZweekdayEnd.val(undefined);
    TZdstSwitchTimeEnd.val(undefined);
  }
}

function timezone_select_update() {
  const selected = $(this).find(":selected");
  const tz_info = selected.data("tz-info");

  if (tz_info) {
    timezone_control_update(tz_info);
    // Refresh the local time without waiting for the next update.
    show_local_time();
    return;
  }

  confirm_prompt(`<p><label for='config-timezone-custom'>Update TZ String:<br>
      <input type='text' id='config-timezone-custom' value='${selected.data("tz-string") || ''}' size='25'>
      </label>`,
    function () {
      const input_tz_string = $('#config-timezone-custom').val();
      const parsed_tz_info = parse_tz_string(input_tz_string);
      if (parsed_tz_info) selected.data("tz-string", input_tz_string);
      else notify_prompt("<p>Not a valid timezone string.");
    }, {
    always: function () {
      timezone_control_update(parse_tz_string(selected.data("tz-string")));
    }
  });
}

function show_local_time() {
  const tz_info = $("#config-timezone-select").find(":selected").data("tz-info");
  const time_span = $("#config-timezone-local-time").find("span");
  if (tz_info) time_span.text(tz_info.time_formatter.format(new Date()));
  else time_span.text("N/A");
}

function init_time_config_dialog() {
  const TZmonthSelects = $("select[id^='config-timezone-dst-'][id$='-month']");
  TZmonthSelects.empty();
  const month_list = get_month_list();
  for (const idx in month_list) {
    const monthOption = $(`<option value="${idx}">${month_list[idx]}</option>`);
    TZmonthSelects.append(monthOption);
  }

  const TZweekNumSelects = $("select[id^='config-timezone-dst-'][id$='-weeknum']");
  TZweekNumSelects.empty();
  const ordinal_list = get_ordinal_list();
  for (const idx in ordinal_list) {
    const ordinal = ordinal_list[idx];
    const ordinalOption = $(`<option value="${ordinal[0]}">${ordinal[1]}</option>`);
    TZweekNumSelects.append(ordinalOption);
  }

  const TZweekdaySelects = $("select[id^='config-timezone-dst-'][id$='-dow']");
  TZweekdaySelects.empty();
  const weekday_list = get_weekday_list();
  for (const idx in weekday_list) {
    const dow = weekday_list[idx];
    const weekdayOption = $(`<option value="${dow[0]}">${dow[1]}</option>`);
    TZweekdaySelects.append(weekdayOption);
  }

  const TZdstSwitchTimeSelects = $("select[id^='config-timezone-dst-'][id$='-time']");
  TZdstSwitchTimeSelects.empty();
  const dst_switch_time = get_dst_switch_time();
  for (const idx in dst_switch_time) {
    const time = dst_switch_time[idx];
    const dstSwitchTimeOption = $(`<option value="${time[0]}">${time[1]}</option>`);
    TZdstSwitchTimeSelects.append(dstSwitchTimeOption);
  }

  const timeZoneSelect = $("#config-timezone-select");
  timeZoneSelect.empty();
  var localTimeZoneOption;
  const tz_list = get_tz_list();
  for (const idx in tz_list) {
    const tz_info = tz_list[idx];
    const timeZoneOption = $(`<option value="${tz_info.id}">${tz_info.display}</option>`);
    if (tz_info.local) timeZoneOption.addClass("tz-local");
    timeZoneOption.data("tz-info", tz_info);
    timeZoneSelect.append(timeZoneOption);
  }
  timeZoneSelect.prepend(`<option value="Custom-Timezone">&lt;&lt; Custom Timezone &gt;&gt;</option>`);
  timeZoneSelect.on('change', timezone_select_update);

  $("#config-ntp-client-enable").on("change", toggle_ntp_client_enable);
  $("#config-timezone-enable").on('change', toggle_timezone_enable);

  // Timezone fields are derived, not customizable.
  $("#config-timezone-utc-offset").prop("disabled", true);
  $("#config-timezone-dst-adjust").prop("disabled", true);
  $("select[id^=config-timezone-dst]").prop("disabled", true);

  setInterval(show_local_time, 1000);
}

function toggle_ntp_client_enable() {
  const ntpServerAddr = $("#config-ntp-server-addr");
  if (this.checked && !ntpServerAddr.val()) {
    ntpServerAddr.val(CONFIG_TIME_DEFAULT_NTP_SERVER);
  }
}

function toggle_timezone_enable() {
  const timeZoneSelect = $("#config-timezone-select");
  if (this.checked && timeZoneSelect.find(":selected").length == 0) {
    timeZoneSelect.find("option.tz-local").prop("selected", "selected");
    timeZoneSelect.trigger('change');
  }
}

function time_setup_save() {
  const DIALOG = $("#config\\:time");
  var config = DIALOG.data('config');

  if ($('#config-ntp-client-enable').is(":checked")) {
    config.ntp_server = $("#config-ntp-server-addr").val();
  } else {
    config.ntp_server = '';
  }

  if ($('#config-timezone-enable').is(":checked")) {
    const selected = $("#config-timezone-select").find(":selected");
    const std_tz_info = selected.data("tz-info");
    if (std_tz_info) config.timezone = std_tz_info.tz_string;
    else config.timezone = selected.data("tz-string");
  } else {
    config.timezone = '';
  }
  console.log("Saving time configuration:", config);

  $.ajax({
    method: "PUT",
    url: URL_CONFIG + '?' + $.param({ "section": CONFIG_SECTION_TIME }),
    data: JSON.stringify(config),
    processData: false,
    contentType: 'application/json',
  }).done(function () {
    notify_prompt("<p>Configuration updated.");
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    notify_prompt(`<p>Configuration update failed${resp_text ? ": " + resp_text : "."}`);
  });
}

function reset_ntp_client_controls() {
  $('#config-ntp-client-enable').removeAttr('checked');
  $('#config-ntp-server-addr').val(undefined);
}

function populate_ntp_client_controls(config) {
  $('#config-ntp-client-enable').attr('checked', 'checked');
  $('#config-ntp-server-addr').val(config['ntp_server']);
}

function reset_timezone_controls() {
  $("#config-timezone-enable").removeAttr("checked");

  $("#config-timezone-select").val(undefined);
  $("#config-timezone-utc-offset").val(undefined);
  $("#config-timezone-dst-select").val(undefined);
  $("select[id^=config-timezone-dst-]").val(undefined);
}

function populate_timezone_controls(config) {
  $('#config-timezone-enable').attr('checked', 'checked');

  const config_tz_string = config['timezone'];
  const parsed_tz_info = parse_tz_string(config_tz_string);
  if (!parsed_tz_info) {
    reset_timezone_controls();
    return;
  }

  const norm_tz_string = get_tz_string(parsed_tz_info);
  const timeZoneSelect = $("#config-timezone-select");
  var std_tz_info;
  timeZoneSelect.find("option").each(function (idx, item) {
    const tz_info = $(item).data("tz-info");
    if (tz_info && tz_info.tz_string == norm_tz_string) {
      std_tz_info = tz_info;
      return false;
    }
  });
  if (std_tz_info) {
    timeZoneSelect.val(std_tz_info.id);
    timeZoneSelect.trigger('change');
  } else {
    timezone_control_update(parsed_tz_info);
    const customTimeZone = timeZoneSelect.find('option[value="Custom-Timezone"]');
    customTimeZone.data("tz-string", norm_tz_string);
    timeZoneSelect.val("Custom-Timezone");
  }
}

function time_setup_dialog_open(config) {
  if (config['ntp_server']) populate_ntp_client_controls(config);
  else reset_ntp_client_controls();

  if (config['timezone']) populate_timezone_controls(config);
  else reset_timezone_controls();

  const DIALOG = $("#config\\:time");
  DIALOG.data('config', config);
  dialog_confirm_prompt(DIALOG, undefined, time_setup_save);
}

const DEBUG_CONFIG_TIME = {
  "baseline": "2025-03-01 10:00:00",
  "timezone": "EST5EDT,M3.2.0/2,M11.1.0",
  "ntp_server": "time.google.com"
};

function time_setup_click() {
  if (DEVMODE) return time_setup_dialog_open(DEBUG_CONFIG_TIME);

  probe_url_for(URL_CONFIG + '?' + $.param({ "section": CONFIG_SECTION_TIME }),
    time_setup_dialog_open, function (text) {
      notify_prompt(`<p>Configuration unavailable:<br>${text}`);
    });

  const DIALOG = $("#config\\:time");
  if (!DIALOG.data('init')) {
    init_time_config_dialog();
    DIALOG.data('init', true);
  }
}

//-----------------------
// Whole page state

function enable_mgmt_functions(boot_serial) {
  BOOT_SERIAL = boot_serial;
  $("#page-content>.card-space>.card").removeClass("disabled");
}

$(function () {
  $("#time-setup").click(time_setup_click);
  $("#storage-backup").click(storage_backup_click);
  $("#storage-restore").click(storage_restore_click);
  $("#storage-reset").click(storage_reset_click);
  $("#reboot").click(reboot_click);

  var drop_recv = $("#storage-file").parent();
  drop_recv.on('drop', drop_storage_file);
  drop_recv.on('dragover', function (evt) {
    evt.preventDefault();
    $(this).addClass('drag-hover');
  });
  drop_recv.on('dragleave', function () {
    $(this).removeClass('drag-hover');
  });
  $("#storage-file").change(storage_file_select);

  if (!DEVMODE) {
    $("#page-content>.card-space>.card").addClass("disabled");
    $("#page-content>.card-space>.card").removeClass('prog-test');

    probe_url_for(URL_BOOT_SERIAL, enable_mgmt_functions, function (text) {
      notify_prompt(`<p>System management feature unavailable: ${text}`);
    });
  } else {
    BOOT_SERIAL = "DEADBEEF";
  }
});