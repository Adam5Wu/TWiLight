#include "Handler_SysFunc_Config.hpp"

#include <stdlib.h>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spi_flash.h"

#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "AppConfig/Interface.hpp"
#include "AppTime/Interface.hpp"

#include "Interface.hpp"

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD-SysCfg";

inline constexpr char FEATURE_PREFIX[] = "/config";
#define URI_PATH_DELIM '/'

inline constexpr char PARAM_SECTION[] = "section";
inline constexpr char SECTION_TIME[] = "time";

using config::AppConfig;

esp_err_t _config_get_section(const std::string& section_name, httpd_req_t* req) {
  utils::AutoReleaseRes<cJSON*> json;
  if (section_name == SECTION_TIME) {
    config::marshal_time(json, config::get()->time);
  } else {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Section not available");
  }

  if (*json == nullptr) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Failed to marshal config section");
  }

  return send_json(req, *json);
}

esp_err_t _config_set_section(const std::string& section_name, httpd_req_t* req) {
  utils::AutoReleaseRes<cJSON*> json;
  ESP_RETURN_ON_ERROR(receive_json(req, json));

  if (section_name == SECTION_TIME) {
    AppConfig::Time time = config::get()->time;
    ESP_GOTO_ON_ERROR(config::parse_time(*json, time), parse_failed);
    config::get()->time = std::move(time);
    ESP_GOTO_ON_ERROR(time::RefreshConfig(), refresh_failed);
  } else {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Section not available");
  }
  if (config::persist() != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to persist config");
  }
  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, NULL, 0));
  return ESP_OK;

parse_failed:
  return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse config section");
refresh_failed:
  return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to refresh config");
}

esp_err_t _handler_config(const char* query_str, httpd_req_t* req) {
  auto section_name = query_parse_param(query_str, PARAM_SECTION);
  if (!section_name) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid parameter");
  }

  switch (req->method) {
    case HTTP_GET:
      return _config_get_section(*section_name, req);
    case HTTP_PUT:
      return _config_set_section(*section_name, req);
    default:
      return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                 "Feature does not accept this method");
  }
}

}  // namespace

bool sysfunc_config(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_PREFIX, utils::STRLEN(FEATURE_PREFIX)) != 0) return false;

  if (esp_err_t err = _handler_config(feature + utils::STRLEN(FEATURE_PREFIX), req);
      err != ESP_OK) {
    ESP_LOGW(TAG, "Config request handler error: %d (0x%x)", err, err);
  }
  return true;
}

}  // namespace zw::esp8266::app::httpd
