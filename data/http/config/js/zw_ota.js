var DEVMODE = window.location.protocol === 'file:';
const URL_OTA_STATE = "/!sys/ota/state";
const URL_OTA_DATA = "/!sys/ota/data";
const URL_OTA_TOGGLE = "/!sys/ota/toggle?";
const URL_REBOOT = "reboot.html";

var FW_UPLOADING = false;

function reboot_link(text) {
  return `<a href="${URL_REBOOT}">${text}</a>`;
}

function drop_file(evt) {
  evt.preventDefault();
  $(this).removeClass('drag-hover');

  this.control.files = evt.originalEvent.dataTransfer.files;
  $(this.control).trigger('change');
}

function file_select(evt) {
  const file = this.files[0];
  if (file) {
    console.log("Selected file:", file);

    const reader = new FileReader();
    reader.onload = process_fw_data;
    reader.readAsArrayBuffer(file);
  }
}

function bad_fw_data_message(detail) {
  return `Not a firmware image or corrupted data${detail ? ":<p>" + detail : "."}`;
}

function process_fw_data(evt) {
  const fw_data = evt.target.result;
  const dataView = new DataView(fw_data);

  const image_magic = dataView.getUint8(0);
  console.log("Image file magic: 0x", image_magic.toString(16));
  if (image_magic != 0xE9) {
    return notify_prompt(bad_fw_data_message("Invalid ESP firmware image magic"));
  }
  const seg1_size = dataView.getUint32(9 + 4 - 1, true);
  console.log("Segment 1 length = ", seg1_size);
  const seg2_ofs = 9 + 8 + seg1_size + 8;
  console.log("Segment 2 starts @ ", seg2_ofs);
  if (seg2_ofs + 256 >= fw_data.byteLength) {
    return notify_prompt(bad_fw_data_message("Invalid RO data segment start"));
  }

  const app_desc_magic = dataView.getUint32(seg2_ofs - 1, true);
  console.log("App desc magic: 0x", app_desc_magic.toString(16));
  if (app_desc_magic != 0xABCD5432) {
    return notify_prompt(bad_fw_data_message("Invalid App descriptor magic"));
  }
  const decoder = new TextDecoder('utf-8');
  const image_version_view = new Uint8Array(fw_data, seg2_ofs + 16 - 1, 32);
  const image_version = decoder.decode(image_version_view);
  const image_name_view = new Uint8Array(fw_data, seg2_ofs + 16 + 32 - 1, 32);
  const image_name = decoder.decode(image_name_view);
  console.log("Image name: ", image_name);
  console.log("Image version: ", image_version);
  const image_time_view = new Uint8Array(fw_data, seg2_ofs + 16 + 64 - 1, 16);
  const image_time = decoder.decode(image_time_view);
  const image_date_view = new Uint8Array(fw_data, seg2_ofs + 16 + 80 - 1, 16);
  const image_date = decoder.decode(image_date_view);
  console.log("Image datetime: ", image_date, image_time);
  const image_idf_view = new Uint8Array(fw_data, seg2_ofs + 16 + 96 - 1, 32);
  const image_idf = decoder.decode(image_idf_view);
  console.log("Image IDF version: ", image_idf);

  var prompt_msg = `Please confirm updating firmware to:
<p class='center'>${image_name} ${image_version}
<span class='nowrap'>(build time ${image_date} ${image_time}</span>`;
  op_confirm_prompt(prompt_msg, send_fw_data, fw_data);
}

function send_progress(evt) {
  if (evt.lengthComputable) {
    var percentComplete = evt.loaded * 100 / evt.total;
    console.log("Upload progress: " + Number.parseFloat(percentComplete).toFixed(2) + '%');

    const dark_scheme = window.matchMedia("(prefers-color-scheme: dark)");
    const prog_lower = Number.parseFloat(percentComplete - 2).toFixed(1);
    const prog_higher = Number.parseFloat(percentComplete + 3).toFixed(1);
    $("#status-text").css("background-image",
      `linear-gradient(70deg, ${dark_scheme ? "teal" : "lightblue"} ${prog_lower}%, transparent ${prog_higher}%)`);
  }
}

function send_fw_data(fw_data) {
  var status_text = $("#status-text");
  status_text.text("Upload in progress, do not interrupt...");
  $("#status").show();
  $("#dropzone").hide();

  FW_UPLOADING = true;
  $.post({
    url: URL_OTA_DATA,
    data: fw_data,
    processData: false,
    contentType: 'application/octet-stream',
    xhr: function () {
      var xhr = new window.XMLHttpRequest();
      xhr.upload.addEventListener("progress", send_progress, false);
      return xhr;
    }
  }).done(function () {
    status_text.html("Upload complete, " + reboot_link("reboot to launch") + ".");
    status_text.css("background-image", "");
    refresh_status(true);
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    status_text.text("Upload failed" + (resp_text ? ": " + resp_text : "."));
  }).always(function () {
    FW_UPLOADING = false;
  });
}

function fw_version_display_text(fw_info) {
  var version = fw_info['image_name'];
  if ('image_ver' in fw_info) version += ' ' + fw_info['image_ver'];
  if ('build_time' in fw_info)
    version += ` <span class="nowrap">(${fw_info['build_time']})</span>`;
  return version;
}

function refresh_status(post_update) {
  var status_text = $("#status-text");
  if (!post_update) {
    status_text.text("Checking OTA status...");
  }

  var fw_list = $("#fw-list");
  fw_list.empty();

  $.when($.getJSON(URL_OTA_STATE)).then(function (ota_states) {
    console.log("Received OTA info:", ota_states);

    for (const idx in ota_states) {
      var fw_info = ota_states[idx];

      var valid = 'image_name' in fw_info;
      var version = valid ? fw_version_display_text(fw_info) : "(unavailable)";
      var status = (idx == 0) ? "boot" : "";
      var next = 'next' in fw_info;
      if (next) status += (status ? ', ' : '') + 'next';
      var fw_row = $(`<tr class="${(next || !valid) ? '' : 'toggle'}">
  <td class="fw-index">${fw_info['index']}</td>
  <td class="fw-info">${version}</span></td>
  <td class="fw-status">${status}</td>
</tr>`);
      fw_row.data("fw_info", fw_info);
      fw_list.append(fw_row);
    }
    if (!post_update) {
      status_text.text("OTA info received.");
      $("#status").hide();
      $("#dropzone").show();
    }
  }, function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    status_text.text("OTA unavailable" + (resp_text ? ": " + resp_text : "."));
  });
}

function select_fw(evt) {
  var fw_row = $(this);
  var fw_info = fw_row.data("fw_info");
  console.log("Selected FW:", fw_info);

  // Ignore operation if firmware uploading in progress
  if (FW_UPLOADING) return;

  // Do nothing if already the next booting image
  if ('next' in fw_info) return;

  if ('image_name' in fw_info) {
    var prompt_message = "Toggle next boot to:<p class='center'>" + fw_version_display_text(fw_info);
    op_confirm_prompt(prompt_message, fw_toggle_proceed, fw_info['index']);
  }
}

function fw_toggle_proceed(index) {
  var status_text = $("#status-text");
  status_text.text("Toggling firmware boot order...");
  $("#status").show();
  $("#dropzone").hide();

  $.get({
    url: URL_OTA_TOGGLE + $.param({ "index": index }),
  }).done(function () {
    status_text.html("Boot image toggled, " + reboot_link("reboot to launch") + ".");
    refresh_status(true);
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    status_text.text("Firmware boot toggle failed" + (resp_text ? ": " + resp_text : "."));
  });
}

$(function () {
  $("#fw-list").on("click", "tr", select_fw);

  var drop_recv = $("#fw-drop");
  drop_recv.on('drop', drop_file);
  drop_recv.on('dragover', function (evt) {
    evt.preventDefault();
    $(this).addClass('drag-hover');
  });
  drop_recv.on('dragleave', function () {
    $(this).removeClass('drag-hover');
  });
  $("#fw-file").change(file_select);

  var ssid_box = $("#ssid");
  var passwd_box = $("#password");
  if (!DEVMODE) {
    // Reset the dev template
    $("#status").show();
    $("#status-text").removeClass('prog-test');
    $("#dropzone").hide();

    refresh_status(false);
  } else {
    var test_info = {
      "index": 0, "image_name": "ESP8266Base", "image_ver": "1.0.0",
      "build_time": "Apr  3 2025 13:56:51", "idf_ver": "v3.4-170-g8e773792-dirty"
    };
    $("#fw-list .toggle").data("fw_info", test_info);
  }
});