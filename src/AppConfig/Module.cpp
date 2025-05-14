#include "Module.hpp"

#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>

#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip_addr.h"

#include "cJSON.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface.hpp"

namespace zw::esp8266::app::config {
namespace {

inline constexpr char TAG[] = "Config";

inline constexpr char BASE_CONFIG_PATH[] = "/config/base.json";
inline constexpr char LIVE_CONFIG_PATH[] = "/app_config.json";

SemaphoreHandle_t access_lock_;
AppConfig app_config_;

std::unordered_map<std::string, GenericFieldHandler> custom_field_handlers_;

//-----------------------
// Parser implementation

utils::DataOrError<size_t> _decode_size(const char* str) {
  if (*str == '\0') return ESP_ERR_NOT_FOUND;
  char* last;
  size_t value = std::strtoul(str, &last, 10);
  if (*last != '\0') {
    ESP_LOGD(TAG, "Size '%s' is malformed", str);
    return ESP_ERR_INVALID_ARG;
  }
  return value;
}

utils::DataOrError<std::optional<ip_addr_t>> _decode_netmask(const char* str) {
  if (*str == '\0') return std::optional<ip_addr_t>(std::nullopt);

  ip_addr_t netmask;
  if (!ipaddr_aton(str, &netmask)) {
    ESP_LOGD(TAG, "Netmask '%s' is malformed", str);
    return ESP_ERR_INVALID_ARG;
  }
  if (!ip_addr_netmask_valid(&netmask)) {
    ESP_LOGD(TAG, "Netmask '%s' is invalid", str);
    return ESP_ERR_INVALID_ARG;
  }
  return std::optional<ip_addr_t>(netmask);
}

#define PARSE_CONFIG_OBJ(json, container, field, parse_name, strict) \
  ESP_RETURN_ON_ERROR(                                               \
      _parse_##parse_name(cJSON_GetObjectItem(json, #field), container.field, strict))

#define PARSE_AND_ASSIGN_FIELD(json, container, field, parse_func, strict) \
  ESP_RETURN_ON_ERROR(parse_and_assign_field(json, #field, container.field, parse_func, strict))

esp_err_t _parse_wifi_ap(const cJSON* json, AppConfig::Wifi::Ap& container, bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, ssid_prefix, string_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, password, string_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, net_provision_only, bool_parser, strict);

  return ESP_OK;
}

esp_err_t _parse_wifi_station(const cJSON* json, AppConfig::Wifi::Station& container,
                              bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, ssid, string_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, password, string_parser, strict);
  return ESP_OK;
}

esp_err_t _parse_wifi(const cJSON* json, AppConfig::Wifi& container, bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, power_saving, bool_parser, strict);
  PARSE_CONFIG_OBJ(json, container, ap, wifi_ap, strict);
  PARSE_CONFIG_OBJ(json, container, station, wifi_station, strict);
  return ESP_OK;
}

esp_err_t _parse_time(const cJSON* json, AppConfig::Time& container, bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, baseline, string_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, timezone, string_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, ntp_server, string_parser, strict);
  return ESP_OK;
}

esp_err_t _parse_dev_mode(const cJSON* json, AppConfig::DevMode& container, bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, web_dav, bool_parser, strict);
  return ESP_OK;
}

esp_err_t _parse_httpd_web_ota(const cJSON* json, AppConfig::HttpServer::WebOTA& container,
                               bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, enabled, bool_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, netmask,
                         (string_decoder<std::optional<ip_addr_t>, _decode_netmask>), strict);
  return ESP_OK;
}

esp_err_t _parse_httpd_net_provision(const cJSON* json,
                                     AppConfig::HttpServer::NetProvision& container,
                                     bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, enabled, bool_parser, strict);
  PARSE_AND_ASSIGN_FIELD(json, container, default_page, string_parser, strict);
  return ESP_OK;
}

esp_err_t _parse_httpd(const cJSON* json, AppConfig::HttpServer& container, bool strict = false) {
  PARSE_AND_ASSIGN_FIELD(json, container, root_dir, string_parser, strict);
  PARSE_CONFIG_OBJ(json, container, net_provision, httpd_net_provision, strict);
  PARSE_CONFIG_OBJ(json, container, web_ota, httpd_web_ota, strict);
  return ESP_OK;
}

esp_err_t _parse(const char* data, AppConfig& container, bool strict) {
  utils::AutoReleaseRes<cJSON*> json(cJSON_ParseWithOpts(data, NULL, strict), [](cJSON* json) {
    if (json) cJSON_Delete(json);
  });
  if (*json == NULL) {
    ESP_LOGW(TAG, "Failed to parse data (around byte %d)", cJSON_GetErrorPtr() - data);
    ESP_LOG_BUFFER_HEXDUMP(TAG, cJSON_GetErrorPtr() - 16, 32, ESP_LOG_DEBUG);
    return ESP_FAIL;
  }

  AppConfig safe_copy = container;
  utils::AutoRelease failsafe([&] { container = std::move(safe_copy); });
  PARSE_CONFIG_OBJ(*json, container, wifi, wifi, strict);
  PARSE_CONFIG_OBJ(*json, container, time, time, strict);
  PARSE_CONFIG_OBJ(*json, container, dev_mode, dev_mode, strict);
  PARSE_CONFIG_OBJ(*json, container, http_server, httpd, strict);

  for (const auto& [key, entry] : custom_field_handlers_) {
    cJSON* item = cJSON_GetObjectItem(*json, key.c_str());
    if (item != NULL) {
      ESP_LOGD(TAG, "Parsing custom field '%s'...", key.c_str());
      ESP_RETURN_ON_ERROR(entry.parse(item, container, strict));
    }
  }

  failsafe.Drop();
  return ESP_OK;
}

//-----------------------
// Logging implementation

#define APPEND_STRINGLIST(str, item)   \
  {                                    \
    if (!str.empty()) str.append(","); \
    str.append(item);                  \
  }

#define STRING_VALUE_OR(str, alt) (str.empty() ? alt : str.c_str())
#define VALUE_IF_EMPTY(val, str) (str.empty() ? val : "")

void _log_dev_mode(const AppConfig::DevMode& container) {
  std::string dev_mode_service;
  if (container.web_dav) APPEND_STRINGLIST(dev_mode_service, "WebDAV");
  ESP_LOGI(TAG, "Development mode: %s", STRING_VALUE_OR(dev_mode_service, "OFF"));
}

void _log_wifi_ap(const AppConfig::Wifi::Ap& container) {
  std::string passwd_disp = utils::PasswordRedact(container.password);
  ESP_LOGI(TAG, "- SoftAP%s:", VALUE_IF_EMPTY(" (Open Access)", passwd_disp));
  ESP_LOGI(TAG, "  Enabled: %s",
           container.net_provision_only ? "Network provision only" : "Always");
  ESP_LOGI(TAG, "  SSID Prefix: %s",
           container.ssid_prefix.empty() ? ZW_APPLIANCE_AP_PREFIX : container.ssid_prefix.c_str());
  if (!passwd_disp.empty()) ESP_LOGI(TAG, "  Password: %s", passwd_disp.c_str());
}

void _log_wifi_station(const AppConfig::Wifi::Station& container) {
  if (container.ssid.empty()) {
    ESP_LOGI(TAG, "- External AP not configured");
  } else {
    std::string passwd_disp = utils::PasswordRedact(container.password);
    ESP_LOGI(TAG, "- External AP%s:", VALUE_IF_EMPTY(" (Open Access)", passwd_disp));
    ESP_LOGI(TAG, "  SSID: %s", container.ssid.c_str());
    if (!passwd_disp.empty()) ESP_LOGI(TAG, "  Password: %s", passwd_disp.c_str());
  }
}

void _log_wifi(const AppConfig::Wifi& container) {
  ESP_LOGI(TAG, "Wi-Fi:");
  ESP_LOGI(TAG, "- Power saving: %s", container.power_saving ? "Enabled" : "Disabled");
  _log_wifi_ap(container.ap);
  _log_wifi_station(container.station);
}

void _log_time(const AppConfig::Time& container) {
  ESP_LOGI(TAG, "Time:");
  ESP_LOGI(TAG, "- Baseline: %s", STRING_VALUE_OR(container.baseline, "(not set)"));
  ESP_LOGI(TAG, "- Timezone: %s", STRING_VALUE_OR(container.timezone, "(not set)"));
  ESP_LOGI(TAG, "- NTP server: %s", STRING_VALUE_OR(container.ntp_server, "(not set)"));
}

void _log_http_net_provision(const AppConfig::HttpServer::NetProvision& container) {
  ESP_LOGI(TAG, "- Net provision %s", container ? "enabled" : "disabled");
  if (container && !container.default_page.empty()) {
    ESP_LOGI(TAG, "  Default page: %s", container.default_page.c_str());
  }
}

void _log_http_web_ota(const AppConfig::HttpServer::WebOTA& container) {
  ESP_LOGI(TAG, "- WebOTA %s", container ? "enabled" : "disabled");
  if (container) {
    std::string netmask("(not specified)");
    if (container.netmask.has_value()) {
      if (ipaddr_ntoa_r(&*container.netmask, &netmask.front(), 16) == NULL) {
        netmask = "(format error)";
      }
    }
    ESP_LOGI(TAG, "  Netmask: %s", netmask.c_str());
  }
}

void _log_http_server(const AppConfig::HttpServer& container) {
  ESP_LOGI(TAG, "HTTP Server:");
  if (!container) {
    ESP_LOGI(TAG, "- Regular serving disabled");
  } else {
    ESP_LOGI(TAG, "- Root directory: %s", container.root_dir.c_str());
  }
  _log_http_net_provision(container.net_provision);
  _log_http_web_ota(container.web_ota);
}

void _log(const AppConfig& container) {
  ESP_LOGI(TAG, "------ Configurations ------");
  _log_dev_mode(container.dev_mode);
  _log_wifi(container.wifi);
  _log_time(container.time);
  _log_http_server(container.http_server);

  for (const auto& [key, entry] : custom_field_handlers_) {
    entry.log(container);
  }

  ESP_LOGI(TAG, "----------------------------");
}

//----------------------------
// Marshalling implementation

std::string _encode_size(const size_t& value) {
  if (value == 0) return "";
  return utils::DataBuf(16).PrintTo("%u", value);
}

std::string _encode_netmask(const std::optional<ip_addr_t>& addr) {
  std::string netmask;
  if (!addr.has_value()) return netmask;

  netmask.resize(15);
  assert(ip4addr_ntoa_r(&addr.value(), &netmask.front(), 16));
  return netmask;
}

#define MARSHAL_CONFIG_OBJ(container, base, update, name, func_suffix) \
  ESP_RETURN_ON_ERROR(                                                 \
      marshal_config_obj(container, #name, base.name, update.name, _marshal_##func_suffix))

#define DIFF_AND_MARSHAL_FIELD(container, base, update, field, marshal_func) \
  ESP_RETURN_ON_ERROR(                                                       \
      diff_and_marshal_field(container, #field, base.field, update.field, marshal_func))

esp_err_t _marshal_wifi_ap(utils::AutoReleaseRes<cJSON*>& container,
                           const AppConfig::Wifi::Ap& base, const AppConfig::Wifi::Ap& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, ssid_prefix, string_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, password, string_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, net_provision_only, bool_marshal);
  return ESP_OK;
}

esp_err_t _marshal_wifi_station(utils::AutoReleaseRes<cJSON*>& container,
                                const AppConfig::Wifi::Station& base,
                                const AppConfig::Wifi::Station& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, ssid, string_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, password, string_marshal);
  return ESP_OK;
}

esp_err_t _marshal_wifi(utils::AutoReleaseRes<cJSON*>& container, const AppConfig::Wifi& base,
                        const AppConfig::Wifi& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, power_saving, bool_marshal);
  MARSHAL_CONFIG_OBJ(container, base, update, ap, wifi_ap);
  MARSHAL_CONFIG_OBJ(container, base, update, station, wifi_station);
  return ESP_OK;
}

esp_err_t _marshal_dev_mode(utils::AutoReleaseRes<cJSON*>& container,
                            const AppConfig::DevMode& base, const AppConfig::DevMode& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, web_dav, bool_marshal);
  return ESP_OK;
}

esp_err_t _marshal_time(utils::AutoReleaseRes<cJSON*>& container, const AppConfig::Time& base,
                        const AppConfig::Time& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, baseline, string_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, timezone, string_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, ntp_server, string_marshal);
  return ESP_OK;
}

esp_err_t _marshal_httpd_net_provision(utils::AutoReleaseRes<cJSON*>& container,
                                       const AppConfig::HttpServer::NetProvision& base,
                                       const AppConfig::HttpServer::NetProvision& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, enabled, bool_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, default_page, string_marshal);
  return ESP_OK;
}

esp_err_t _marshal_httpd_web_ota(utils::AutoReleaseRes<cJSON*>& container,
                                 const AppConfig::HttpServer::WebOTA& base,
                                 const AppConfig::HttpServer::WebOTA& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, enabled, bool_marshal);
  DIFF_AND_MARSHAL_FIELD(container, base, update, netmask,
                         (string_encoder<std::optional<ip_addr_t>, _encode_netmask>));
  return ESP_OK;
}

esp_err_t _marshal_httpd(utils::AutoReleaseRes<cJSON*>& container,
                         const AppConfig::HttpServer& base, const AppConfig::HttpServer& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, root_dir, string_marshal);
  MARSHAL_CONFIG_OBJ(container, base, update, net_provision, httpd_net_provision);
  MARSHAL_CONFIG_OBJ(container, base, update, web_ota, httpd_web_ota);
  return ESP_OK;
}

esp_err_t _marshal(utils::AutoReleaseRes<cJSON*>& container, const AppConfig& base,
                   const AppConfig& update) {
  MARSHAL_CONFIG_OBJ(container, base, update, wifi, wifi);
  MARSHAL_CONFIG_OBJ(container, base, update, dev_mode, dev_mode);
  MARSHAL_CONFIG_OBJ(container, base, update, time, time);
  MARSHAL_CONFIG_OBJ(container, base, update, http_server, httpd);

  for (const auto& [key, entry] : custom_field_handlers_) {
    ESP_RETURN_ON_ERROR(marshal_config_obj(container, key.c_str(), base, update, entry.marshal));
  }

  return ESP_OK;
}

//---------------------------
// Config storage operations
//---------------------------

esp_err_t _load_config(const std::string& file_path, AppConfig& config) {
  utils::AutoReleaseRes<FILE*> file(fopen(file_path.c_str(), "r"), [](FILE* file) {
    if (file) fclose(file);
  });
  if (*file == NULL) {
    ESP_LOGE(TAG, "Failed to open file");
    return ESP_FAIL;
  }
  struct stat st;
  if (fstat(fileno(*file), &st) != 0) {
    ESP_LOGE(TAG, "Failed to query file stats");
    return ESP_ERR_NOT_FOUND;
  }
  ESP_LOGD(TAG, "Reading %ld bytes...", st.st_size);
  utils::DataBuf buffer(st.st_size + 1);
  ESP_RETURN_ON_ERROR((fread(&buffer.front(), 1, st.st_size, *file) == st.st_size) ? ESP_OK
                                                                                   : ESP_FAIL);
  buffer[st.st_size] = 0;
  return _parse((const char*)&buffer.front(), config, false);
}

esp_err_t _store_config(const std::string& file_path, cJSON* json) {
  utils::AutoReleaseRes<char*> config_str(cJSON_Print(json), [](char* data) {
    if (data) cJSON_free(data);
  });
  if (*config_str == NULL) {
    ESP_LOGE(TAG, "Failed to print JSON data diff");
    return ESP_FAIL;
  }
  size_t diff_len = strlen(*config_str);
#ifndef NDEBUG
  ESP_LOGI(TAG, "Config diff (%d bytes):", diff_len);
  ESP_LOG_BUFFER_HEXDUMP(TAG, *config_str, diff_len, ESP_LOG_DEBUG);
#endif

  {
    utils::AutoReleaseRes<FILE*> file(fopen(file_path.c_str(), "w"), [](FILE* file) {
      if (file) fclose(file);
    });
    if (fwrite(*config_str, 1, diff_len, *file) != diff_len) {
      ESP_LOGW(TAG, "Failed to write config file");
      return ESP_FAIL;
    }
  }
  ESP_LOGI(TAG, "Saved config (%d bytes) to %s", diff_len, file_path.c_str());

  return ESP_OK;
}

}  // namespace

XAppConfig get() {
// Since we are wait forever, we don't expect it to fail.
#ifdef ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK
  xSemaphoreTakeRecursive(access_lock_, portMAX_DELAY);
#else
  xSemaphoreTake(access_lock_, portMAX_DELAY);
#endif
  return {[] {
#ifdef ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK
            xSemaphoreGiveRecursive(access_lock_);
#else
            xSemaphoreGive(access_lock_);
#endif
          },
          app_config_};
}

esp_err_t persist() {
// Since we are wait forever, we don't expect it to fail.
#ifdef ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK
  xSemaphoreTakeRecursive(access_lock_, portMAX_DELAY);
#else
  xSemaphoreTake(access_lock_, portMAX_DELAY);
#endif
  utils::AutoRelease access_unlocker([] {
#ifdef ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK
    xSemaphoreGiveRecursive(access_lock_);
#else
    xSemaphoreGive(access_lock_);
#endif
  });

  AppConfig base_config;
  {
    ESP_LOGD(TAG, "Loading system base config...");
    std::string config_path(ZW_SYSTEM_MOUNT_POINT);
    config_path.append(BASE_CONFIG_PATH);
    ESP_RETURN_ON_ERROR(_load_config(config_path, base_config));
  }

  {
    utils::AutoReleaseRes<cJSON*> json;
    ESP_LOGD(TAG, "Marshalling config...");
    ESP_RETURN_ON_ERROR(_marshal(json, base_config, app_config_));
    if (*json == NULL) {
      ESP_LOGD(TAG, "New config matches baseline!");
      // Create an empty root object
      ESP_RETURN_ON_ERROR(allocate_container(json));
    }
    std::string config_path(ZW_STORAGE_MOUNT_POINT);
    config_path.append(LIVE_CONFIG_PATH);
    return _store_config(config_path, *json);
  }
}

//--------------------------
// Data field parsing utils
//--------------------------

utils::DataOrError<std::string> string_parser(cJSON* item) {
  if (cJSON_IsString(item)) return std::string(cJSON_GetStringValue(item));
  return ESP_ERR_INVALID_ARG;
}

utils::DataOrError<bool> bool_parser(cJSON* item) {
  if (cJSON_IsTrue(item)) return true;
  if (cJSON_IsFalse(item)) return false;
  return ESP_ERR_INVALID_ARG;
}

#define EXPORT_DECODE_UTIL(type, func_name) \
  utils::DataOrError<type> decode_##func_name(const char* str) { return _decode_##func_name(str); }

EXPORT_DECODE_UTIL(size_t, size);
EXPORT_DECODE_UTIL(std::optional<ip_addr_t>, netmask);

#undef EXPORT_DECODE_UTIL

#define EXPORT_PARSE_FUNC(type, func_name)                                    \
  esp_err_t parse_##func_name(const cJSON* json, type& config, bool strict) { \
    return _parse_##func_name(json, config, strict);                          \
  }

EXPORT_PARSE_FUNC(AppConfig::Wifi::Station, wifi_station);
EXPORT_PARSE_FUNC(AppConfig::Time, time);
// EXPORT_PARSE_FUNC(AppConfig::Wifi::Ap, wifi_ap);
#undef EXPORT_PARSE_FUNC

//------------------------------
// Data field marshalling utils
//------------------------------

utils::DataOrError<cJSON*> string_marshal(const std::string& base, const std::string& update) {
  if (base == update) return (cJSON*)NULL;
  if (cJSON* result = cJSON_CreateString(update.c_str())) return result;
  return ESP_ERR_NO_MEM;
}

utils::DataOrError<cJSON*> bool_marshal(const bool& base, const bool& update) {
  if (base == update) return (cJSON*)NULL;
  if (cJSON* result = cJSON_CreateBool(update)) return result;
  return ESP_ERR_NO_MEM;
}

#define EXPORT_ENCODE_UTIL(type, func_name) \
  std::string encode_##func_name(const type& value) { return _encode_##func_name(value); }

EXPORT_ENCODE_UTIL(size_t, size);
EXPORT_ENCODE_UTIL(std::optional<ip_addr_t>, netmask);

#undef EXPORT_ENCODE_UTIL

esp_err_t allocate_container(utils::AutoReleaseRes<cJSON*>& container) {
  if (*container != NULL) return ESP_OK;

  container = utils::AutoReleaseRes<cJSON*>(cJSON_CreateObject(), [](cJSON* json) {
    if (json) cJSON_Delete(json);
  });
  if (*container == NULL) {
    ESP_LOGE(TAG, "Failed to allocate cJSON object");
    return ESP_FAIL;
  }
  return ESP_OK;
}

#define EXPORT_MARSHAL_FUNC(type, func_name)                                         \
  esp_err_t marshal_##func_name(utils::AutoReleaseRes<cJSON*>& json, type& config) { \
    return _marshal_##func_name(json, type(), config);                               \
  }

EXPORT_MARSHAL_FUNC(AppConfig::Time, time);
// EXPORT_MARSHAL_FUNC(AppConfig::Wifi::Ap, wifi_ap);
#undef EXPORT_MARSHAL_FUNC

esp_err_t register_field(const std::string& key, GenericFieldHandler&& handler) {
  auto insert_ret = custom_field_handlers_.emplace(key, std::move(handler));
  if (!insert_ret.second) {
    ESP_LOGW(TAG, "Field '%s' already registered", key.c_str());
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t init() {
  {
    ESP_LOGD(TAG, "Creating access lock...");
    access_lock_ =
#ifdef ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK
        xSemaphoreCreateRecursiveMutex();
#else
        xSemaphoreCreateMutex();
#endif
    ESP_RETURN_ON_ERROR((access_lock_ == NULL) ? ESP_ERR_NO_MEM : ESP_OK);
  }

  {
    ESP_LOGD(TAG, "Loading system base config...");
    std::string config_path(ZW_SYSTEM_MOUNT_POINT);
    config_path.append(BASE_CONFIG_PATH);
    ESP_RETURN_ON_ERROR(_load_config(config_path, app_config_));
#ifndef NDEBUG
    _log(app_config_);
#endif
  }
  {
    ESP_LOGD(TAG, "Loading live config...");
    std::string config_path(ZW_STORAGE_MOUNT_POINT);
    config_path.append(LIVE_CONFIG_PATH);
    if (_load_config(config_path, app_config_) != ESP_OK) {
      ESP_LOGW(TAG, "Unable to load live config!");
    }
    _log(app_config_);
  }

  return ESP_OK;
}

void finit(void) {
  // Take the config access lock, to avoid incomplete updates.
  xSemaphoreTake(access_lock_, portMAX_DELAY);
}

}  // namespace zw::esp8266::app::config