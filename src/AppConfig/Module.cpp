#include "Module.hpp"

#include <string>
#include <memory>
#include <vector>
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

// Parse JSON serialized AppConfig in `data`, and store in provided `config`.
// Fields in `config` will be left untouched, unless new values are
// specified in `data`. So one can build layered configs by repeatedly
// parsing different `data` with the same `config`.
//
// If returns failure, input `config` will be kept unchanged.
// Note the the parsing is fairly lenient, individual bad entries are
// simply ignored (e.g. unexpected data type). Only serious issues (e.g.
// malformed JSON structure) will cause the whole function to fail.
esp_err_t _parse(const char* data, AppConfig& config, bool strict = false);

// Print configuration in debug log.
void _log(const AppConfig& config);

//-----------------------
// Parser implementation

#define PARSE_AND_ASSIGN_STRING_ENCODED_FIELD(json, config, field, decoder, strict) \
  {                                                                                 \
    cJSON* item = cJSON_GetObjectItem(json, #field);                                \
    if (strict && !cJSON_IsString(item)) return ESP_ERR_INVALID_ARG;                \
    if (const char* v = cJSON_GetStringValue(item)) {                               \
      if (auto val = decoder(v); !val) {                                            \
        if (strict) return ESP_ERR_INVALID_ARG;                                     \
      } else {                                                                      \
        config.field = std::move(*val);                                             \
      }                                                                             \
    }                                                                               \
  }

utils::DataOrError<ip_addr_t> decode_netmask_(const char* str) {
  if (*str == '\0') return ESP_ERR_NOT_FOUND;
  ip_addr_t netmask;
  if (!ipaddr_aton(str, &netmask)) {
    ESP_LOGD(TAG, "Netmask '%s' is malformed", str);
    return ESP_ERR_INVALID_ARG;
  }
  if (!ip_addr_netmask_valid(&netmask)) {
    ESP_LOGD(TAG, "Netmask '%s' is invalid", str);
    return ESP_ERR_INVALID_ARG;
  }
  return netmask;
}

#define PARSE_AND_ASSIGN_STRING(json, config, field, strict)          \
  {                                                                   \
    cJSON* item = cJSON_GetObjectItem(json, #field);                  \
    if (strict && !cJSON_IsString(item)) return ESP_ERR_INVALID_ARG;  \
    if (const char* v = cJSON_GetStringValue(item)) config.field = v; \
  }

#define PARSE_AND_ASSIGN_BOOL(json, config, field, strict)         \
  {                                                                \
    const cJSON* item = cJSON_GetObjectItem(json, #field);         \
    if (strict && !cJSON_IsBool(item)) return ESP_ERR_INVALID_ARG; \
    if (cJSON_IsTrue(item))                                        \
      config.field = true;                                         \
    else if (cJSON_IsFalse(item))                                  \
      config.field = false;                                        \
  }

#define PARSE_CONFIG_OBJ(json, config, field, parse_name, strict) \
  ESP_RETURN_ON_ERROR(_parse_##parse_name(cJSON_GetObjectItem(json, #field), config.field, strict))

esp_err_t _parse_wifi_ap(const cJSON* json, AppConfig::Wifi::Ap& config, bool strict = false) {
  PARSE_AND_ASSIGN_STRING(json, config, ssid_prefix, strict);
  PARSE_AND_ASSIGN_STRING(json, config, password, strict);
  PARSE_AND_ASSIGN_BOOL(json, config, net_provision_only, strict);

  return ESP_OK;
}

esp_err_t _parse_wifi_station(const cJSON* json, AppConfig::Wifi::Station& config,
                              bool strict = false) {
  PARSE_AND_ASSIGN_STRING(json, config, ssid, strict);
  PARSE_AND_ASSIGN_STRING(json, config, password, strict);
  return ESP_OK;
}

esp_err_t _parse_wifi(const cJSON* json, AppConfig::Wifi& config, bool strict = false) {
  PARSE_AND_ASSIGN_BOOL(json, config, power_saving, strict);
  PARSE_CONFIG_OBJ(json, config, ap, wifi_ap, strict);
  PARSE_CONFIG_OBJ(json, config, station, wifi_station, strict);
  return ESP_OK;
}

esp_err_t _parse_time(const cJSON* json, AppConfig::Time& config, bool strict = false) {
  PARSE_AND_ASSIGN_STRING(json, config, baseline, strict);
  PARSE_AND_ASSIGN_STRING(json, config, ntp_server, strict);
  return ESP_OK;
}

esp_err_t _parse_dev_mode(const cJSON* json, AppConfig::DevMode& config, bool strict = false) {
  PARSE_AND_ASSIGN_BOOL(json, config, web_dav, strict);
  return ESP_OK;
}

esp_err_t _parse_httpd_web_ota(const cJSON* json, AppConfig::HttpServer::WebOTA& config,
                               bool strict = false) {
  PARSE_AND_ASSIGN_BOOL(json, config, enabled, strict);
  PARSE_AND_ASSIGN_STRING_ENCODED_FIELD(json, config, netmask, decode_netmask_, strict);
  return ESP_OK;
}

esp_err_t _parse_httpd_net_provision(const cJSON* json, AppConfig::HttpServer::NetProvision& config,
                                     bool strict = false) {
  PARSE_AND_ASSIGN_BOOL(json, config, enabled, strict);
  PARSE_AND_ASSIGN_STRING(json, config, default_page, strict);
  return ESP_OK;
}

esp_err_t _parse_httpd(const cJSON* json, AppConfig::HttpServer& config, bool strict = false) {
  PARSE_AND_ASSIGN_STRING(json, config, root_dir, strict);
  PARSE_CONFIG_OBJ(json, config, net_provision, httpd_net_provision, strict);
  PARSE_CONFIG_OBJ(json, config, web_ota, httpd_web_ota, strict);
  return ESP_OK;
}

esp_err_t _parse(const char* data, AppConfig& config, bool strict) {
  utils::AutoReleaseRes<cJSON*> json(cJSON_ParseWithOpts(data, NULL, strict), [](cJSON* json) {
    if (json) cJSON_Delete(json);
  });
  if (*json == nullptr) {
    ESP_LOGW(TAG, "Failed to parse data (around byte %d)", cJSON_GetErrorPtr() - data);
    ESP_LOG_BUFFER_HEXDUMP(TAG, cJSON_GetErrorPtr() - 16, 32, ESP_LOG_DEBUG);
    return ESP_FAIL;
  }

  AppConfig safe_copy = config;
  utils::AutoRelease failsafe([&] { config = std::move(safe_copy); });
  PARSE_CONFIG_OBJ(*json, config, wifi, wifi, strict);
  PARSE_CONFIG_OBJ(*json, config, time, time, strict);
  PARSE_CONFIG_OBJ(*json, config, dev_mode, dev_mode, strict);
  PARSE_CONFIG_OBJ(*json, config, http_server, httpd, strict);
  failsafe.Drop();
  return ESP_OK;
}

//-----------------------
// Logger implementation

#define APPEND_STRINGLIST(str, item)   \
  {                                    \
    if (!str.empty()) str.append(","); \
    str.append(item);                  \
  }

#define STRING_VALUE_OR(str, alt) (str.empty() ? alt : str.c_str())
#define VALUE_IF_EMPTY(val, str) (str.empty() ? val : "")

void _log_dev_mode(const AppConfig::DevMode& config) {
  std::string dev_mode_service;
  if (config.web_dav) APPEND_STRINGLIST(dev_mode_service, "WebDAV");
  ESP_LOGI(TAG, "Development mode: %s", STRING_VALUE_OR(dev_mode_service, "OFF"));
}

void _log_wifi_ap(const AppConfig::Wifi::Ap& config) {
  std::string passwd_disp = utils::PasswordRedact(config.password);
  ESP_LOGI(TAG, "- SoftAP%s:", VALUE_IF_EMPTY(" (Open Access)", passwd_disp));
  ESP_LOGI(TAG, "  Enabled: %s", config.net_provision_only ? "Network provision only" : "Always");
  ESP_LOGI(TAG, "  SSID Prefix: %s",
           config.ssid_prefix.empty() ? ZW_APPLIANCE_AP_PREFIX : config.ssid_prefix.c_str());
  if (!passwd_disp.empty()) ESP_LOGI(TAG, "  Password: %s", passwd_disp.c_str());
}

void _log_wifi_station(const AppConfig::Wifi::Station& config) {
  if (config.ssid.empty()) {
    ESP_LOGI(TAG, "- External AP not configured");
  } else {
    std::string passwd_disp = utils::PasswordRedact(config.password);
    ESP_LOGI(TAG, "- External AP%s:", VALUE_IF_EMPTY(" (Open Access)", passwd_disp));
    ESP_LOGI(TAG, "  SSID: %s", config.ssid.c_str());
    if (!passwd_disp.empty()) ESP_LOGI(TAG, "  Password: %s", passwd_disp.c_str());
  }
}

void _log_wifi(const AppConfig::Wifi& config) {
  ESP_LOGI(TAG, "Wi-Fi:");
  ESP_LOGI(TAG, "- Power saving: %s", config.power_saving ? "Enabled" : "Disabled");
  _log_wifi_ap(config.ap);
  _log_wifi_station(config.station);
}

void _log_time(const AppConfig::Time& config) {
  ESP_LOGI(TAG, "Time:");
  ESP_LOGI(TAG, "- Baseline: %s", STRING_VALUE_OR(config.baseline, "(not set)"));
  ESP_LOGI(TAG, "- NTP server: %s", STRING_VALUE_OR(config.ntp_server, "(not set)"));
}

void _log_http_net_provision(const AppConfig::HttpServer::NetProvision& config) {
  ESP_LOGI(TAG, "- Net provision %s", config ? "enabled" : "disabled");
  if (config && !config.default_page.empty()) {
    ESP_LOGI(TAG, "  Default page: %s", config.default_page.c_str());
  }
}

void _log_http_web_ota(const AppConfig::HttpServer::WebOTA& config) {
  ESP_LOGI(TAG, "- Web WebOTA %s", config ? "enabled" : "disabled");
  if (config) {
    std::string netmask;
    if (!config.netmask.has_value()) {
      netmask = "(not specified)";
    } else {
      netmask.resize(16);
      assert(ip4addr_ntoa_r(&config.netmask.value(), &netmask.front(), 17));
    }
    ESP_LOGI(TAG, "  Netmask: %s", netmask.c_str());
  }
}

void _log_http_server(const AppConfig::HttpServer& config) {
  ESP_LOGI(TAG, "HTTP Server:");
  if (!config) {
    ESP_LOGI(TAG, "- Regular serving disabled");
  } else {
    ESP_LOGI(TAG, "- Root directory: %s", config.root_dir.c_str());
  }
  _log_http_net_provision(config.net_provision);
  _log_http_web_ota(config.web_ota);
}

void _log(const AppConfig& config) {
  ESP_LOGI(TAG, "------ Configurations ------");
  _log_dev_mode(config.dev_mode);
  _log_wifi(config.wifi);
  _log_time(config.time);
  _log_http_server(config.http_server);
  ESP_LOGI(TAG, "----------------------------");
}

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
  ESP_LOGI(TAG, "Reading %ld bytes...", st.st_size);
  utils::DataBuf buffer(st.st_size + 1);
  ESP_RETURN_ON_ERROR((fread(&buffer.front(), 1, st.st_size, *file) == st.st_size) ? ESP_OK
                                                                                   : ESP_FAIL);
  buffer[st.st_size] = 0;
  return _parse((const char*)&buffer.front(), config);
}

esp_err_t _need_obj(utils::AutoReleaseRes<cJSON*>& container) {
  if (*container != nullptr) return ESP_OK;

  container = utils::AutoReleaseRes<cJSON*>(cJSON_CreateObject(), [](cJSON* json) {
    if (json) cJSON_Delete(json);
  });

  if (*container == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate cJSON object");
    return ESP_FAIL;
  }
  return ESP_OK;
}
#define DIFF_CONFIG_OBJ(container, base, update, name, diff_name)          \
  {                                                                        \
    utils::AutoReleaseRes<cJSON*> _node;                                   \
    ESP_RETURN_ON_ERROR(_diff_##diff_name(_node, base.name, update.name)); \
    if (*_node != nullptr) {                                               \
      ESP_RETURN_ON_ERROR(_need_obj(container));                           \
      cJSON_AddItemToObject(*container, #name, _node.Drop());              \
    }                                                                      \
  }

#define DIFF_BOOL(container, base, update, name)             \
  {                                                          \
    if (base.name != update.name) {                          \
      ESP_RETURN_ON_ERROR(_need_obj(container));             \
      cJSON_AddBoolToObject(*container, #name, update.name); \
    }                                                        \
  }

#define DIFF_STRING(container, base, update, name)                     \
  {                                                                    \
    if (base.name != update.name) {                                    \
      ESP_RETURN_ON_ERROR(_need_obj(container));                       \
      cJSON_AddStringToObject(*container, #name, update.name.c_str()); \
    }                                                                  \
  }

#define DIFF_STRING_ENCODED(container, base, update, name, encoder)   \
  {                                                                   \
    std::string base_val = encoder(base.name);                        \
    std::string update_val = encoder(update.name);                    \
    if (base_val != update_val) {                                     \
      ESP_RETURN_ON_ERROR(_need_obj(container));                      \
      cJSON_AddStringToObject(*container, #name, update_val.c_str()); \
    }                                                                 \
  }

std::string encode_netmask_(const std::optional<ip_addr_t>& addr) {
  std::string netmask;
  if (!addr.has_value()) return netmask;
  netmask.resize(16);
  assert(ip4addr_ntoa_r(&addr.value(), &netmask.front(), 17));
  return netmask;
}

esp_err_t _diff_wifi_ap(utils::AutoReleaseRes<cJSON*>& node, const AppConfig::Wifi::Ap& base,
                        const AppConfig::Wifi::Ap& update) {
  DIFF_STRING(node, base, update, ssid_prefix);
  DIFF_STRING(node, base, update, password);
  DIFF_BOOL(node, base, update, net_provision_only);
  return ESP_OK;
}

esp_err_t _diff_wifi_station(utils::AutoReleaseRes<cJSON*>& node,
                             const AppConfig::Wifi::Station& base,
                             const AppConfig::Wifi::Station& update) {
  DIFF_STRING(node, base, update, ssid);
  DIFF_STRING(node, base, update, password);
  return ESP_OK;
}

esp_err_t _diff_wifi(utils::AutoReleaseRes<cJSON*>& node, const AppConfig::Wifi& base,
                     const AppConfig::Wifi& update) {
  DIFF_BOOL(node, base, update, power_saving);
  DIFF_CONFIG_OBJ(node, base, update, ap, wifi_ap);
  DIFF_CONFIG_OBJ(node, base, update, station, wifi_station);
  return ESP_OK;
}

esp_err_t _diff_dev_mode(utils::AutoReleaseRes<cJSON*>& node, const AppConfig::DevMode& base,
                         const AppConfig::DevMode& update) {
  DIFF_BOOL(node, base, update, web_dav);
  return ESP_OK;
}

esp_err_t _diff_time(utils::AutoReleaseRes<cJSON*>& node, const AppConfig::Time& base,
                     const AppConfig::Time& update) {
  DIFF_STRING(node, base, update, baseline);
  DIFF_STRING(node, base, update, ntp_server);
  return ESP_OK;
}

esp_err_t _diff_httpd_net_provision(utils::AutoReleaseRes<cJSON*>& node,
                                    const AppConfig::HttpServer::NetProvision& base,
                                    const AppConfig::HttpServer::NetProvision& update) {
  DIFF_BOOL(node, base, update, enabled);
  DIFF_STRING(node, base, update, default_page);
  return ESP_OK;
}

esp_err_t _diff_httpd_web_ota(utils::AutoReleaseRes<cJSON*>& node,
                              const AppConfig::HttpServer::WebOTA& base,
                              const AppConfig::HttpServer::WebOTA& update) {
  DIFF_BOOL(node, base, update, enabled);
  DIFF_STRING_ENCODED(node, base, update, netmask, encode_netmask_);
  return ESP_OK;
}

esp_err_t _diff_httpd(utils::AutoReleaseRes<cJSON*>& node, const AppConfig::HttpServer& base,
                      const AppConfig::HttpServer& update) {
  DIFF_STRING(node, base, update, root_dir);
  DIFF_CONFIG_OBJ(node, base, update, net_provision, httpd_net_provision);
  DIFF_CONFIG_OBJ(node, base, update, web_ota, httpd_web_ota);
  return ESP_OK;
}

esp_err_t _diff(utils::AutoReleaseRes<cJSON*>& node, const AppConfig& base,
                const AppConfig& update) {
  DIFF_CONFIG_OBJ(node, base, update, wifi, wifi);
  DIFF_CONFIG_OBJ(node, base, update, dev_mode, dev_mode);
  DIFF_CONFIG_OBJ(node, base, update, time, time);
  DIFF_CONFIG_OBJ(node, base, update, http_server, httpd);

  return ESP_OK;
}

esp_err_t _store(cJSON* json) {
  utils::AutoReleaseRes<char*> config_str(cJSON_Print(json), [](char* data) {
    if (data) cJSON_free(data);
  });
  if (*config_str == nullptr) {
    ESP_LOGE(TAG, "Failed to print JSON data diff");
    return ESP_FAIL;
  }
  size_t diff_len = strlen(*config_str);
#ifndef NDEBUG
  ESP_LOGI(TAG, "Config diff (%d bytes):", diff_len);
  ESP_LOG_BUFFER_HEXDUMP(TAG, *config_str, diff_len, ESP_LOG_DEBUG);
#endif

  std::string config_path(ZW_STORAGE_MOUNT_POINT);
  config_path.append(LIVE_CONFIG_PATH);
  {
    utils::AutoReleaseRes<FILE*> file(fopen(config_path.c_str(), "w"), [](FILE* file) {
      if (file) fclose(file);
    });
    if (fwrite(*config_str, 1, diff_len, *file) != diff_len) {
      ESP_LOGW(TAG, "Failed to write config file");
      return ESP_FAIL;
    }
  }
  ESP_LOGI(TAG, "Saved config (%d bytes) to %s", diff_len, config_path.c_str());

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
    ESP_LOGI(TAG, "Loading system base config...");
    std::string config_path(ZW_SYSTEM_MOUNT_POINT);
    config_path.append(BASE_CONFIG_PATH);
    ESP_RETURN_ON_ERROR(_load_config(config_path, base_config));
  }

  utils::AutoReleaseRes<cJSON*> json;
  ESP_RETURN_ON_ERROR(_diff(json, base_config, app_config_));
  if (*json == nullptr) {
    ESP_LOGI(TAG, "New config matches baseline!");
    // Create an empty root object
    ESP_RETURN_ON_ERROR(_need_obj(json));
  }

  return _store(*json);
}

#define EXPORT_PARSE_FUNC(type, func_name)                       \
  esp_err_t parse_##func_name(const cJSON* json, type& config) { \
    return _parse_##func_name(json, config, /*strict=*/true);    \
  }

EXPORT_PARSE_FUNC(AppConfig::Wifi::Station, wifi_station);
// EXPORT_PARSE_FUNC(AppConfig::Wifi::Ap, wifi_ap);
// EXPORT_PARSE_FUNC(AppConfig::Time, time);
#undef EXPORT_PARSE_FUNC

#define EXPORT_MARSHAL_FUNC(type, func_name)                                         \
  esp_err_t marshal_##func_name(utils::AutoReleaseRes<cJSON*>& json, type& config) { \
    return _diff_##func_name(json, type(), config);                                  \
  }

// EXPORT_MARSHAL_FUNC(AppConfig::Time, time);
#undef EXPORT_MARSHAL_FUNC

esp_err_t init() {
  {
    ESP_LOGI(TAG, "Creating access lock...");
    access_lock_ =
#ifdef ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK
        xSemaphoreCreateRecursiveMutex();
#else
        xSemaphoreCreateMutex();
#endif
    ESP_RETURN_ON_ERROR((access_lock_ == NULL) ? ESP_ERR_NO_MEM : ESP_OK);
  }

  {
    ESP_LOGI(TAG, "Loading system base config...");
    std::string config_path(ZW_SYSTEM_MOUNT_POINT);
    config_path.append(BASE_CONFIG_PATH);
    ESP_RETURN_ON_ERROR(_load_config(config_path, app_config_));
#ifndef NDEBUG
    _log(app_config_);
#endif
  }
  {
    ESP_LOGI(TAG, "Loading live config...");
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