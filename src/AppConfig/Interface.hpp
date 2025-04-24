
#ifndef APPCONFIG_INTERFACE
#define APPCONFIG_INTERFACE

#include <string>
#include <optional>
#include <memory>

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
    std::string baseline;

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

// Parse a cJSON object into a config object
#define DEFINE_PARSE_FUNC(type, func_name) \
  extern esp_err_t parse_##func_name(const cJSON* json, type& config)

DEFINE_PARSE_FUNC(AppConfig::Wifi::Station, wifi_station);
// DEFINE_PARSE_FUNC(AppConfig::Wifi::Ap, wifi_ap);
// DEFINE_PARSE_FUNC(AppConfig::Time, time);
#undef DEFINE_PARSE_FUNC

// Marshal a config object into a cJSON object
#define DEFINE_MARSHAL_FUNC(type, func_name) \
  extern esp_err_t marshal_##func_name(utils::AutoReleaseRes<cJSON*>& json, type& config)

// DEFINE_MARSHAL_FUNC(AppConfig::Time, time);
// DEFINE_MARSHAL_FUNC(AppConfig::Wifi::Ap, wifi_ap);
#undef DEFINE_MARSHAL_FUNC

}  // namespace zw::esp8266::app::config

#endif  // APPCONFIG_INTERFACE
