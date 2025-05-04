var DEVMODE = window.location.protocol === 'file:';
const URL_STORAGE = "/!sys/storage";
const URL_REBOOT = "reboot.html";

var BOOT_SERIAL;

function reboot_click() {
  if (!BOOT_SERIAL) return;
  confirm_prompt("<p>Confirm reboot?", do_reboot);
}

function do_reboot() {
  window.location.href = URL_REBOOT;
}

function storage_backup_click() {
  var download_link = document.createElement('a');
  download_link.href = URL_STORAGE;
  download_link.setAttribute('download', '');
  download_link.target = 'download-frame';
  download_link.click();
}

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
  $.post({
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

function enable_mgmt_functions(boot_serial) {
  BOOT_SERIAL = boot_serial;

  $("#storage-backup").removeClass("disabled");
  $("#storage-restore").removeClass("disabled");
  $("#storage-reset").removeClass("disabled");
  $("#reboot").removeClass("disabled");
}

$(function () {
  $("#reboot").click(reboot_click);
  $("#storage-backup").click(storage_backup_click);
  $("#storage-restore").click(storage_restore_click);
  $("#storage-reset").click(storage_reset_click);

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
    $("#storage-backup").removeClass('prog-test');
    $("#storage-backup").addClass("disabled");
    $("#storage-restore").addClass("disabled");
    $("#storage-reset").addClass("disabled");
    $("#reboot").addClass("disabled");

    probe_url_for(URL_BOOT_SERIAL, enable_mgmt_functions, function (text) {
      notify_prompt(`<p>System management feature unavailable: ${text}`);
    });
  } else {
    BOOT_SERIAL = "DEADBEEF";
  }
});