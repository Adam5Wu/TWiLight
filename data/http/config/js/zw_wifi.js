var DEVMODE = window.location.protocol === 'file:';
const URL_STA_APLIST = "/!sys/prov/sta.aplist";
const URL_STA_STATE = "/!sys/prov/sta.state";
const URL_STA_CONFIG = "/!sys/prov/sta.config";

function toggle_passwd(evt) {
  var toggle = $(this);
  var passwd_box = $("#password");
  // This is *before* the toggle actually happen!
  if (toggle.prop('checked')) {
    passwd_box.attr("type", "text");
  } else {
    passwd_box.attr("type", "password");
  }
}

function send_credential(evt) {
  var ssid_box = $("#ssid");
  var passwd_box = $("#password");

  var payload = {
    "ssid": ssid_box.val(),
    "password": passwd_box.val()
  };
  $("#btn-apply").prop('disabled', true);
  $.ajax({
    type: "PUT",
    url: URL_STA_CONFIG,
    data: JSON.stringify(payload),
    contentType: 'application/json'
  }).done(function () {
    notify_prompt("<p>Configuration applied!<p>Please wait for the device to connect...");
  }).fail(function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined')
      ? jqXHR.responseText : "(Response text unavailable)";
    notify_prompt(`<p>Failed to apply configuration.<p>${resp_text}`);
  }).always(function () {
    $("#btn-apply").prop('disabled', false);
  });
}

function apply_credential(evt) {
  evt.preventDefault();

  var passwd_box = $("#password");
  if (passwd_box.val() == passwd_box.data('reset')) {
    return notify_prompt("<p>Current configured password has been redacted for privacy.<p>Please re-type.");
  }

  var prompt_message = `<p>The network connection to your device will be interrupted momentarily.
    <p>If the credential doesn't work, the device will come back to provisioning mode.`;
  confirm_prompt(prompt_message, send_credential);
}

function select_ap(evt) {
  var ap_row = $(this);
  var ap_info = ap_row.data("ap_info");
  console.log("Selected AP:", ap_info);

  var ssid_box = $("#ssid");
  var passwd_box = $("#password");
  if (ap_row.hasClass('selected')) {
    ap_row.removeClass('selected');
    ssid_box.val(ssid_box.data("reset"));
    passwd_box.val(passwd_box.data("reset"));
  } else {
    $("#wifi-list tr.selected").removeClass('selected');
    ap_row.addClass('selected');
    ssid_box.val(ap_info['ssid']);
    if (ap_info['open']) {
      passwd_box.val("");
      passwd_box.prop("required", false);
      passwd_box.prop("disabled", true);
    } else {
      passwd_box.prop("required", true);
      passwd_box.prop("disabled", false);
    }
    if (ap_info['ssid']) {
      if (!ap_info['open']) {
        passwd_box.focus();
        passwd_box.select();
      }
    } else {
      ssid_box.focus();
    }
  }
}

function refresh_aplist() {
  $("#btn-rescan").prop('disabled', true);
  $("#wifi-list").html(`<tr>
  <td class="wifi-index"></td>
  <td class="wifi-ssid">&#x27F3; Scanning...</td>
  <td class="wifi-signal"></td>
</tr>`);

  $.when($.getJSON(URL_STA_APLIST)).then(function (ap_list) {
    console.log("Received AP list:", ap_list);

    var wifi_list = $("#wifi-list");
    wifi_list.empty();
    var ssid_val = $("#ssid").val();
    var count = 0;
    for (const idx in ap_list) {
      var ap = ap_list[idx];

      var ap_bars = 4;
      var rssi = ap['rssi'];
      if (rssi < -60) ap_bars--;
      if (rssi < -70) ap_bars--;
      if (rssi < -80) ap_bars--;

      var ap_name = ap['ssid'] || "(Hidden)";
      var ssid_class = [];
      if (!ap['open']) ssid_class.push("auth");
      if (!ap['ssid']) ssid_class.push("noname");

      var tr_class = [];
      if (ap['ssid'] && ap['ssid'] == ssid_val) {
        tr_class.push("selected");
      }

      var ap_row = $(`<tr class="${tr_class.join(' ')}">
  <td class="wifi-index disp"></td>
  <td class="wifi-ssid ${ssid_class.join(' ')}">${ap_name}</td>
  <td class="wifi-signal signal-${ap_bars}"></td>
</tr>`);
      ap_row.data("ap_info", ap);
      wifi_list.append(ap_row);

      if (++count >= 16) break;
    }
  }, function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    $("#wifi-list td.wifi-ssid").text("Failed to list APs" + (resp_text ? ": " + resp_text : ""));
  }).done(function () {
    $("#btn-rescan").prop('disabled', false);
  })
}

function refresh_station() {
  var status_box = $("#status-text");
  status_box.text("Fetching connection state...");

  var ssid_box = $("#ssid");
  var passwd_box = $("#password");
  ssid_box.val("");
  passwd_box.val("");

  $.when($.getJSON(URL_STA_STATE)).then(function (sta_info) {
    console.log("Received Station info:", sta_info);
    var config = sta_info['config'];
    var connection = sta_info['connection'];
    if ('channel' in connection) {
      status_box.text(`Connected to '${connection['ssid']}' on channel ${connection['channel']}`);
      ssid_box.val(connection['ssid']);
      ssid_box.data("reset", connection['ssid']);
    } else {
      status_box.text("Not connected to an AP.");
      ssid_box.val(config['ssid']);
      ssid_box.data("reset", config['ssid']);
    }
    passwd_box.val(config['password']);
    passwd_box.data("reset", config['password']);

    ssid_box.prop('disabled', false);
    passwd_box.prop('disabled', false);
    $("#btn-apply").prop('disabled', false);
  }, function (jqXHR) {
    var resp_text = (typeof jqXHR.responseText !== 'undefined') ? jqXHR.responseText : "";
    $("#status-text").text("Unable to query connection state" + (resp_text ? ": " + resp_text : ""));
  }).done(refresh_aplist);
}

$(function () {
  $("#reveal-passwd").click(toggle_passwd);
  $("#btn-rescan").click(refresh_aplist);
  $("#wifi-form").submit(apply_credential);
  $("#wifi-list").on("click", "tr", select_ap);

  var ssid_box = $("#ssid");
  var passwd_box = $("#password");
  if (!DEVMODE) {
    // Reset the dev template
    $("#status-text").empty();
    $("#wifi-list").empty();
    $("#btn-rescan").prop('disabled', true);
    $("#btn-apply").prop('disabled', true);

    ssid_box.val("");
    ssid_box.prop('disabled', true);
    passwd_box.val("");
    passwd_box.prop('disabled', true);

    refresh_station();
  } else {
    passwd_box.data('reset', passwd_box.val());
  }
});