#include "Handler_SysFunc_Provision.hpp"

#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"

#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface_Private.hpp"
#include "AppConfig/Interface.hpp"
#include "AppNetwork/Interface.hpp"

#include "Interface.hpp"

#ifdef ZW_APPLIANCE_COMPONENT_WEB_NET_PROVISION

#define PROV_APLIST_MAX_LEN 16

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD-SYS-PROV";

inline constexpr char FEATURE_PREFIX[] = "/prov";
#define URI_PATH_DELIM '/'

inline constexpr char PROV_STA_STATE[] = "/sta.state";
inline constexpr char PROV_STA_CONFIG[] = "/sta.config";
inline constexpr char PROV_STA_APLIST[] = "/sta.aplist";

using config::AppConfig;

esp_err_t _prov_sta_aplist_get(httpd_req_t* req) {
  std::string ap_list_json = "[";
  {
    ESP_LOGI(TAG, "Scanning APs...");
    if (serving_config().provisioning) {
      // A reconnect attempt may be ongoing
      // Let's stop that since we have someone looking to reconfigure
      ESP_RETURN_ON_ERROR(esp_wifi_disconnect());
    }

    wifi_scan_config_t params = {};
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&params, true));

    uint16_t ap_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count));
    {
      std::vector<wifi_ap_record_t> ap_list_buffer(ap_count);
      ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&ap_count, &ap_list_buffer.front()));
      utils::DataBuf ap_entry;
      for (uint16_t i = 0; i < std::min(ap_count, (uint16_t)PROV_APLIST_MAX_LEN); i++) {
        const wifi_ap_record_t& ap = ap_list_buffer[i];
        ESP_LOGD(TAG, "%2d. %-32s (%d dBm)", i + 1, ap.ssid, ap.rssi);

        ap_list_json.append("\n ").append(ap_entry.PrintTo(
            R"json({"ssid":"%s","channel":%d,"rssi":%d,"open":%s},)json", ap.ssid, ap.primary,
            ap.rssi, ap.authmode == WIFI_AUTH_OPEN ? "true" : "false"));
      }
    }
  }
  ap_list_json.pop_back();
  ap_list_json.append("\n]");

  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTPD_TYPE_JSON));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, ap_list_json.data(), ap_list_json.length()));
  return ESP_OK;
}

esp_err_t _prov_sta_state_get(httpd_req_t* req) {
  std::string sta_state = "{\n ";
  utils::DataBuf sta_fmtbuf;
  {
    AppConfig::Wifi::Station config = config::get()->wifi.station;
    std::string passwd_disp = utils::PasswordRedact(config.password);
    sta_state
        .append(sta_fmtbuf.PrintTo(R"json("config":{"ssid":"%.32s","password":"%.32s"},)json",
                                   config.ssid.c_str(), passwd_disp.c_str()))
        .append("\n ");
  }
  {
    wifi_ap_record_t ap_record;
    sta_state.append(R"json("connection":{)json");
    if (esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
      sta_state.append(sta_fmtbuf.PrintTo(R"json("ssid":"%.32s","channel":%d)json", ap_record.ssid,
                                          ap_record.primary));
    }
    sta_state.append("}\n");
  }
  sta_state.push_back('}');
  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTPD_TYPE_JSON));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, sta_state.data(), sta_state.length()));
  return ESP_OK;
}

esp_err_t _prov_sta_config_set(httpd_req_t* req) {
  if (req->content_len > 26 + 32 + 32) {
    return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Configuration oversize");
  }
  utils::AutoReleaseRes<cJSON*> json;
  ESP_RETURN_ON_ERROR(receive_json(req, json));

  AppConfig::Wifi::Station sta_config = config::get()->wifi.station;
  if (config::parse_wifi_station(*json, sta_config) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload failed to parse as config");
  }
  config::get()->wifi.station = std::move(sta_config);
  if (config::persist() != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to persist config");
  }
  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, NULL, 0));
  network::ApplyStationConfig();
  return ESP_OK;
}

esp_err_t _handler_provision(const char* feature, httpd_req_t* req) {
  if (*feature == '\0' || *feature != URI_PATH_DELIM) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed request");
  }

  if (feature) {
    if (strcmp(feature, PROV_STA_APLIST) == 0) {
      switch (req->method) {
        case HTTP_GET:
          return _prov_sta_aplist_get(req);
        default:
          goto method_not_allowed;
      }
    } else if (strcmp(feature, PROV_STA_STATE) == 0) {
      switch (req->method) {
        case HTTP_GET:
          return _prov_sta_state_get(req);
        default:
          goto method_not_allowed;
      }
    } else if (strcmp(feature, PROV_STA_CONFIG) == 0) {
      switch (req->method) {
        case HTTP_PUT:
          return _prov_sta_config_set(req);
        default:
          goto method_not_allowed;
      }
    }
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Feature not available");

method_not_allowed:
  return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                             "Feature does not accept this method");
}

}  // namespace

bool sysfunc_provision(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_PREFIX, utils::STRLEN(FEATURE_PREFIX)) != 0) return false;

  if (esp_err_t err = _handler_provision(feature + utils::STRLEN(FEATURE_PREFIX), req);
      err != ESP_OK) {
    ESP_LOGW(TAG, "Provisioning request handler error: %d (0x%x)", err, err);
  }
  return true;
}

}  // namespace zw::esp8266::app::httpd

#endif  // ZW_APPLIANCE_COMPONENT_WEB_NET_PROVISION