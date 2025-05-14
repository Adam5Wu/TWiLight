
#ifndef APPCONFIG_INTERFACE
#define APPCONFIG_INTERFACE

#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <optional>
#include <functional>

#include "esp_err.h"
#include "esp_log.h"

#include "cJSON.h"
#include "lwip/ip_addr.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

namespace zw::esp8266::app::config {

struct AppConfig {
  struct Wifi {
    // Whether enables power saving feature (may reduce throughput).
    bool power_saving;
    struct Ap {
      // The prefix of the SoftAP SSID.
      // Full SSID will contain a unique ID of the chip.
      std::string ssid_prefix = ZW_APPLIANCE_AP_PREFIX;

      // Password for the SoftAP.
      // Empty will create an open AP.
      std::string password;

      // Whether the SoftAP is only for network provisioning.
      // If set to true, SoftAP will be disabled when the system successfully
      // connects an external AP in station mode.
      bool net_provision_only = true;
    } ap;

    struct Station {
      // The SSID of the external AP.
      std::string ssid;

      // Password for the SoftAP.
      // Empty if it is an open AP.
      std::string password;

      // Station is provisioned if SSID is set.
      operator bool() const { return !ssid.empty(); }
      void clear() { ssid.clear(), password.clear(); }
    } station;
  } wifi;

  struct Time {
    // Optional UTC time to set as the baseline when the appliance boots up.
    // Format: YYYY-MM-DD HH:MM:SS
    std::string baseline;

    // POSIX TZ string for timezone specification
    std::string timezone;

    // Optional NTP server address to sync time from (if connected to AP).
    std::string ntp_server;
  } time;

  // When dev mode is enabled, the system and data partitions will be made
  // accessible under "/.fs/" virtual directory of the respective service.
  // Additionally, the system partition, which is normally mounted read-only,
  // will be re-mounted read/write.
  struct DevMode {
    // Enable WebDAV for development.
    bool web_dav;

    // Dev mode is enabled if any of the development services is enabled.
    operator bool() const { return web_dav; }
  } dev_mode;

  struct HttpServer {
    // The root directory for HTTP serving.
    // An empty value will *disable* the service under *regular* operation.
    // (Only enabled for network provisioning and development).
    std::string root_dir;

    struct NetProvision {
      // During network provisioning, the HTTP server will always accept ESP Wifi
      // protocomm traffic. So no Web-based network provision is a valid option
      // (just less user-friendly).
      // Note that, if set net provision handler will be enabled *whenever* the
      // HTTP server is on (not just during no-AP net provisioning).
      bool enabled;

      // The path to net provision page, relative to the root dir.
      // If set, during network provisioning, it will be served as the default page
      // (as opposed to "index.html").
      std::string default_page;

      operator bool() const { return enabled; }
    } net_provision;

    struct WebOTA {
      // Enable OTA firmware update.
      bool enabled;

      // Restrict upload client network.
      std::optional<ip_addr_t> netmask;

      operator bool() const { return enabled; }
    } web_ota;

    // Regular HTTP service is enabled if the root directory is set.
    operator bool() const { return !root_dir.empty(); }
  } http_server;
};

// AppConfig reference with exclusive access.
// Provides concurrent access protection.
class XAppConfig : protected utils::AutoRelease {
  using _this = XAppConfig;

 public:
  // Cannot copy-construct or copy-assign.
  XAppConfig(const _this&) = delete;
  XAppConfig& operator=(const _this&) = delete;

  const AppConfig& operator*() const { return config_; }
  const AppConfig* operator->() const { return &config_; }

  AppConfig& operator*() { return config_; }
  AppConfig* operator->() { return &config_; }

 protected:
  friend XAppConfig get();
  XAppConfig(ReleaseFunc&& func, AppConfig& config)
      : utils::AutoRelease(std::move(func)), config_(config) {}

  AppConfig& config_;
};

// Get a reference to the config data with a locked mutex.
//
// While the returned object is alive, *all* other concurrent accesses
// from other tasks will be *blocked*. Avoid holding it for a long time!!
extern XAppConfig get(void);

// Write the current config into the file system.
extern esp_err_t persist(void);

//--------------------------
// Data field parsing utils
//--------------------------

// If JSON field is absent, do not touch `config_field`;
// If JSON field data is valid, overwrite `config_field`;
// If JSON field data is malformed:
// - do not touch `config_field` if `strict` is false;
// - Otherwise returns error.
template <typename T>
extern esp_err_t parse_and_assign_field(const cJSON* json, const char* field, T& config_field,
                                        utils::DataOrError<T> (*parser)(cJSON*),
                                        bool strict = false) {
  cJSON* item = cJSON_GetObjectItem(json, field);
  if (item != NULL) {
    if (auto result = parser(item); result) {
      config_field = std::move(*result);
    } else {
      if (strict) return result.error();
    }
  }
  return ESP_OK;
}

extern utils::DataOrError<std::string> string_parser(cJSON* item);
extern utils::DataOrError<bool> bool_parser(cJSON* item);

template <typename T, utils::DataOrError<T> (*decoder)(const char*)>
extern utils::DataOrError<T> string_decoder(cJSON* item) {
  return cJSON_IsString(item) ? decoder(cJSON_GetStringValue(item))
                              : utils::DataOrError<T>(ESP_ERR_INVALID_ARG);
}

#define DEFINE_DECODE_UTIL(type, func_name) \
  extern utils::DataOrError<type> decode_##func_name(const char* str)

DEFINE_DECODE_UTIL(size_t, size);
DEFINE_DECODE_UTIL(std::optional<ip_addr_t>, netmask);

#undef DEFINE_DECODE_UTIL

template <typename T, const std::unordered_map<std::string_view, T>& map>
utils::DataOrError<T> decode_enum(const char* str) {
  auto it = map.find(str);
  if (it == map.end()) return ESP_ERR_INVALID_ARG;
  return T(it->second);
}

// Parse a cJSON object into a config object
#define DEFINE_PARSE_FUNC(type, func_name) \
  extern esp_err_t parse_##func_name(const cJSON* json, type& config, bool strict = false)

DEFINE_PARSE_FUNC(AppConfig::Wifi::Station, wifi_station);
DEFINE_PARSE_FUNC(AppConfig::Time, time);
// DEFINE_PARSE_FUNC(AppConfig::Wifi::Ap, wifi_ap);
#undef DEFINE_PARSE_FUNC

//------------------------------
// Data field marshalling utils
//------------------------------

esp_err_t allocate_container(utils::AutoReleaseRes<cJSON*>& container);

// The configs are always marshalled on the differences from a base.
// If full marshalling is desired, pass an empty base.
//
// The marshal function returns:
// - NULL if no update is detected;
// - A cJSON item for marshalled update;
// - Error code if something went wrong;
template <typename T>
extern esp_err_t diff_and_marshal_field(utils::AutoReleaseRes<cJSON*>& container, const char* field,
                                        const T& base, const T& update,
                                        utils::DataOrError<cJSON*> (*marshal)(const T&, const T&)) {
  ASSIGN_OR_RETURN(auto val, marshal(base, update));
  if (val != NULL) {
    utils::AutoReleaseRes<cJSON*> item(std::move(val), [](cJSON* json) {
      if (json != NULL) cJSON_Delete(json);
    });
    if (esp_err_t err = allocate_container(container); err != ESP_OK) return err;
    cJSON_AddItemToObject(*container, field, item.Drop());
  }
  return ESP_OK;
}

template <typename T, typename MarshalFuncType>
extern esp_err_t marshal_config_obj(utils::AutoReleaseRes<cJSON*>& container, const char* field,
                                    const T& base, const T& update,
                                    const MarshalFuncType& marshal) {
  utils::AutoReleaseRes<cJSON*> _node;
  if (esp_err_t err = marshal(_node, base, update); err != ESP_OK) return err;
  if (*_node != NULL) {
    if (esp_err_t err = allocate_container(container); err != ESP_OK) return err;
    cJSON_AddItemToObject(*container, field, _node.Drop());
  }
  return ESP_OK;
}

extern utils::DataOrError<cJSON*> string_marshal(const std::string& base,
                                                 const std::string& update);
extern utils::DataOrError<cJSON*> bool_marshal(const bool& value, const bool& update);

template <typename T, std::string (*encoder)(const T&)>
extern utils::DataOrError<cJSON*> string_encoder(const T& base, const T& update) {
  auto base_value = encoder(base);
  auto update_value = encoder(update);
  if (base_value == update_value) return (cJSON*)NULL;
  if (cJSON* result = cJSON_CreateString(update_value.c_str())) return result;
  return ESP_ERR_NO_MEM;
}

#define DEFINE_ENCODE_UTIL(type, func_name) extern std::string encode_##func_name(const type&)

DEFINE_ENCODE_UTIL(size_t, size);
DEFINE_ENCODE_UTIL(std::optional<ip_addr_t>, netmask);

#undef DEFINE_ENCODE_UTIL

template <typename T, const std::unordered_map<T, std::string_view>& map>
std::string encode_enum(const T& val) {
  auto it = map.find(val);
  if (it == map.end()) return "?";
  return std::string(it->second);
}

// Marshal a config object into a cJSON object
#define DEFINE_MARSHAL_FUNC(type, func_name) \
  extern esp_err_t marshal_##func_name(utils::AutoReleaseRes<cJSON*>& json, type& config)

DEFINE_MARSHAL_FUNC(AppConfig::Time, time);
// DEFINE_MARSHAL_FUNC(AppConfig::Wifi::Ap, wifi_ap);
#undef DEFINE_MARSHAL_FUNC

struct GenericFieldHandler {
  std::function<esp_err_t(const cJSON*, AppConfig&, bool)> parse;
  std::function<void(const AppConfig&)> log;
  std::function<esp_err_t(utils::AutoReleaseRes<cJSON*>&, const AppConfig&, const AppConfig&)>
      marshal;
};
extern esp_err_t register_field(const std::string& key, GenericFieldHandler&& handler);

template <typename ConfigType>
esp_err_t register_custom_field(
    const std::string& key, ConfigType& (*field_getter)(AppConfig& config),
    esp_err_t (*parse)(const cJSON*, ConfigType&, bool), void (*log)(const ConfigType&),
    esp_err_t (*marshal)(utils::AutoReleaseRes<cJSON*>&, const ConfigType&, const ConfigType&)) {
  return register_field(
      key,
      GenericFieldHandler{
          [=](const cJSON* json, AppConfig& config, bool strict) {
            return parse(json, field_getter(config), strict);
          },
          [=](const AppConfig& config) { log(field_getter(const_cast<AppConfig&>(config))); },
          [=](utils::AutoReleaseRes<cJSON*>& json, const AppConfig base, const AppConfig& update) {
            return marshal(json, field_getter(const_cast<AppConfig&>(base)),
                           field_getter(const_cast<AppConfig&>(update)));
          },
      });
}

}  // namespace zw::esp8266::app::config

#endif  // APPCONFIG_INTERFACE
