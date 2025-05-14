#include "Module.hpp"

#include <string>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"
#include "ZWAppUtils.hpp"

#include "Interface_Private.hpp"
#include "CaptiveDNS.hpp"

#include "AppConfig/Interface.hpp"
#include "AppEventMgr/Interface.hpp"
#include "AppHTTPD/Interface.hpp"

#define HTTPD_STARTUP_TIMEOUT (3 * CONFIG_FREERTOS_HZ)  // 3 sec

#define STATION_WAIT_CYCLES 10                            // 10 sec
#define PROVISION_WAIT_INTERVAL (5 * CONFIG_FREERTOS_HZ)  // 5 sec

#define STATION_RECONNECT_BACKOFF 5  // 5 sec

namespace zw::esp8266::app::network {
namespace {

inline constexpr char TAG[] = "Network";

using config::AppConfig;

InternalStates states_;

std::string _get_ap_name(const AppConfig::Wifi::Ap& config) {
  std::string result = config.ssid_prefix;
  if (result.empty()) result = ZW_APPLIANCE_AP_PREFIX;
  char id_str[7] = {0};

  uint8_t efuse_mac[6];
  ESP_GOTO_ON_ERROR(esp_efuse_mac_get_default(&efuse_mac[0]), random_id);
  sprintf(&id_str[0], "%02x%02x%02x", efuse_mac[0] ^ efuse_mac[3], efuse_mac[1] ^ efuse_mac[4],
          efuse_mac[2] ^ efuse_mac[5]);
  result.append(&id_str[0]);
  ESP_LOGD(TAG, "Derived SoftAP SSID: %s", result.c_str());
  return result;

random_id:
  uint32_t rand_str = esp_random();
  sprintf(&id_str[0], "%06x", rand_str & 0xffffff);
  result.append(&id_str[0]);
  ESP_LOGD(TAG, "Fallback random SoftAP SSID: %s", result.c_str());
  return result;
}

#define PROVISION_AUTH_FAIL BIT0
#define PROVISION_AUTH_PASS BIT1
#define PROVISION_STOPPED BIT2

void _provision_event(void* user_data, wifi_prov_cb_event_t event, void* event_data) {
  EventGroupHandle_t provision_events = (EventGroupHandle_t)user_data;
  switch (event) {
#ifdef ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS
    case WIFI_PROV_START:
      if (xTaskCreate(ZWTaskWrapper<TAG, captive_dns_task>, "zw_wifi_captivedns", 1200, NULL, 5,
                      NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Captive DNS worker!");
        eventmgr::SetSystemFailed();
      }
      break;
#endif
    case WIFI_PROV_CRED_FAIL:
      xEventGroupSetBits(provision_events, PROVISION_AUTH_FAIL);
      break;
    case WIFI_PROV_CRED_SUCCESS:
      xEventGroupSetBits(provision_events, PROVISION_AUTH_PASS);
      break;
    case WIFI_PROV_END:
      xEventGroupSetBits(provision_events, PROVISION_STOPPED);
      break;

    default:
      // Not interested in other events.
      break;
  }
}

esp_err_t _provision_do(const AppConfig::Wifi& config) {
  utils::AutoReleaseRes<EventGroupHandle_t> provision_events(xEventGroupCreate(),
                                                             [](EventGroupHandle_t h) {
                                                               if (h) vEventGroupDelete(h);
                                                             });
  if (*provision_events == NULL) {
    ESP_LOGE(TAG, "Failed to allocate provision event group");
    return ESP_FAIL;
  }

  while (true) {
    ESP_LOGD(TAG, "Initialize provision manager...");
    {
      wifi_prov_mgr_config_t mgr_config = {
          .scheme = wifi_prov_scheme_softap,
          .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
          .app_event_handler = {.event_cb = _provision_event, .user_data = *provision_events},
          // We wish to manage Wifi by ourselves.
          .wifi_touch_free = true,
      };
      wifi_prov_scheme_softap_set_httpd_handle(httpd::handle());
      ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(mgr_config));
    }
    do {
      utils::AutoRelease _wifi_prov_mgr_deinit([] {
        // Always tear down and re-setup, even if we just need a new credential.
        // This is because the provision manager cannot accept a retry.
        ESP_LOGD(TAG, "Turning down provision manager...");
        wifi_prov_mgr_deinit();
      });

      ESP_LOGI(TAG, "Starting provisioning...");
      ESP_RETURN_ON_ERROR(wifi_prov_mgr_start_provisioning(
          WIFI_PROV_SECURITY_1, NULL, states_.APName.c_str(),
          config.ap.password.empty() ? NULL : config.ap.password.c_str()));

      if (config.station) {
        // Wifi station is configured, we are in a proactive provision session.
        // The Wifi station is already configured, we just need to start retrying...
        ESP_LOGD(TAG, "Retrying current station configuration...");
        ESP_RETURN_ON_ERROR(esp_wifi_connect());
      }

      while (true) {
        EventBits_t provision_state =
            xEventGroupWaitBits(*provision_events, PROVISION_AUTH_FAIL | PROVISION_AUTH_PASS, false,
                                false, PROVISION_WAIT_INTERVAL);
        if (provision_state & PROVISION_AUTH_PASS) {
          ESP_LOGD(TAG, "Station connected!");
          // Give time for the provision client to see the status
          wifi_prov_mgr_wait();
          goto prov_done;
        } else if (provision_state & PROVISION_AUTH_FAIL) {
          ESP_LOGW(TAG, "Credential did not work, will restart provisioning...");
          // Reset event bits for next provision
          xEventGroupClearBits(*provision_events, PROVISION_AUTH_FAIL);
          // Give time for the provision client to see the status
          sleep(CONFIG_WIFI_PROV_AUTOSTOP_TIMEOUT);
          ESP_LOGD(TAG, "Stopping provision...");
          wifi_prov_mgr_stop_provisioning();
          break;
        }
        ESP_LOGD(TAG, "Provision in progress...");
      }

      // We are here because provision authentication failed.
      while (true) {
        // Give the async cleanup task time to complete.
        EventBits_t provision_state = xEventGroupWaitBits(*provision_events, PROVISION_STOPPED,
                                                          false, false, PROVISION_WAIT_INTERVAL);
        if (provision_state & PROVISION_STOPPED) break;
        ESP_LOGD(TAG, "Waiting for provision manager...");
      }
    } while (0);
  }

prov_done:
  std::string ssid(32, '\0');
  std::string password(64, '\0');
  {
    wifi_config_t wifi_config;
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config));

    strncpy(&ssid.front(), (const char*)&wifi_config.sta.ssid[0], 32);
    ssid.resize(strlen(ssid.c_str()));
    strncpy(&password.front(), (const char*)&wifi_config.sta.password[0], 64);
    password.resize(strlen(password.c_str()));
  }

  {
    auto new_config = config::get();
    if (new_config->wifi.station.ssid == ssid && new_config->wifi.station.password == password) {
      ESP_LOGD(TAG, "WiFi station credential unchanged...");
    } else {
      ESP_LOGD(TAG, "Storing WiFi station credential...");
      new_config->wifi.station.ssid = std::move(ssid);
      new_config->wifi.station.password = std::move(password);
      ESP_RETURN_ON_ERROR(config::persist());
    }
  }

  return ESP_OK;
}

void _station_got_ip_event(const ip_event_got_ip_t& event) {
  ESP_LOGD(TAG, "WiFi station received IP: %s", ipaddr_ntoa(&event.ip_info.ip));
  states_.StationIPAddr = event.ip_info.ip;
}

#define STATION_BAD_AUTH BIT0
#define STATION_CONNECTED BIT1
#define STATION_IP_READY BIT2

void _station_connect_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                    void* event_data) {
  EventGroupHandle_t station_events = (EventGroupHandle_t)arg;
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGD(TAG, "WiFi station connected!");
        xEventGroupSetBits(station_events, STATION_CONNECTED);
        break;

      case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGD(TAG, "WiFi station disconnect: %d", disconnected->reason);
        if (disconnected->reason == WIFI_REASON_AUTH_EXPIRE ||
            disconnected->reason == WIFI_REASON_AUTH_FAIL) {
          xEventGroupSetBits(station_events, STATION_BAD_AUTH);
        }
        // Other disconnect reasons are transient, ignore and keep retrying.
        esp_wifi_connect();
      } break;

      default:
        // Not interested in other events.
        break;
    }
  } else {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP:
        _station_got_ip_event(*(ip_event_got_ip_t*)event_data);
        xEventGroupSetBits(station_events, STATION_IP_READY);
        break;

      default:
        // Not interested in other events.
        break;
    }
  }
}

esp_err_t _station_do_connect(const AppConfig::Wifi::Station& config) {
  ESP_LOGI(TAG, "Connecting to AP '%s'...", config.ssid.c_str());
  wifi_config_t wifi_config = {};
  strlcpy((char*)wifi_config.sta.ssid, config.ssid.c_str(), 32);
  strlcpy((char*)wifi_config.sta.password, config.password.c_str(), 64);

  ESP_RETURN_ON_ERROR(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  return esp_wifi_connect();
}

esp_err_t _station_try_connect(const AppConfig::Wifi::Station& config) {
  wifi_ap_record_t ap_record;
  if (esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
    ESP_LOGD(TAG, "Already connected to AP '%s'...", (const char*)ap_record.ssid);
    if (config.ssid == (const char*)ap_record.ssid) {
      // We must come from a finished provisioning session, which will only
      // exit after getting an IP assignment, so we just signal as such.
      tcpip_adapter_ip_info_t ip_info;
      ESP_RETURN_ON_ERROR(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
      states_.StationIPAddr = ip_info.ip;

      eventmgr::system_states_set(ZW_SYSTEM_STATE_NET_STA_IP_READY);
      eventmgr::system_event_post(ZW_SYSTEM_EVENT_NET_STA_IP_READY);
      return ESP_OK;
    }
    ESP_LOGD(TAG, "Different AP configured, disconnecting...");
    ESP_RETURN_ON_ERROR(esp_wifi_disconnect());
  }

  utils::AutoReleaseRes<EventGroupHandle_t> station_events(xEventGroupCreate(), [](EventGroupHandle_t h) {
    if (h) vEventGroupDelete(h);
  });
  if (*station_events == NULL) {
    ESP_LOGE(TAG, "Failed to allocate station event group");
    return ESP_FAIL;
  }

  utils::AutoRelease _event_handler_unregister([] {
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _station_connect_event_handler);
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, _station_connect_event_handler);
  });
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 _station_connect_event_handler, *station_events));
  ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                 _station_connect_event_handler, *station_events));

  _station_do_connect(config);

  ESP_LOGD(TAG, "Waiting for connection...");
  for (int i = 0; i < STATION_WAIT_CYCLES; i++) {
    ZW_WAIT_FOR_EVENTS(*station_events, STATION_IP_READY | STATION_BAD_AUTH, /*reset=*/false,
                       /*all=*/false, CONFIG_FREERTOS_HZ, break,
                       ESP_LOGD(TAG, "Connection in progress..."););
  }

  EventBits_t station_state = xEventGroupGetBits(*station_events);
  if (station_state & STATION_IP_READY) {
    ESP_LOGD(TAG, "WiFi station connection successful");
    eventmgr::system_states_set(ZW_SYSTEM_STATE_NET_STA_IP_READY);
    eventmgr::system_event_post(ZW_SYSTEM_EVENT_NET_STA_IP_READY);
  } else if (station_state & STATION_CONNECTED) {
    // Could not get IP address, something is wrong
    ESP_LOGW(TAG, "Timeout getting IP assignment...");
    // The station config may still be valid, just that the
    // DHCP service is (temporarily) unavailable.
    // Let's disconnect and enter proactive provisioning...
    ESP_RETURN_ON_ERROR(esp_wifi_disconnect());
  } else if (station_state & STATION_BAD_AUTH) {
    ESP_LOGE(TAG, "WiFi station authentication failed");
    ESP_RETURN_ON_ERROR(esp_wifi_disconnect());
    // No need to keep retying, and clear credential in config.
    config::get()->wifi.station.clear();
  } else {
    ESP_LOGW(TAG, "WiFi station connection timeout");
    // The station config may still be valid, just temporarily unavailable.
  }
  // Note that in no cases we clear the credential in storage.
  // Storage is only updated at a successful provisioning.
  return ESP_OK;
}

esp_err_t _wifi_provision(const AppConfig::Wifi& config) {
  // Not needed, provision manager soft AP module will set this.
  // ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), failed);

  if (!config.station) {
    ESP_LOGD(TAG, "WiFi station not configured, start provisioning...");
  } else {
    ESP_LOGD(TAG, "Start proactive provisioning...");
  }

  // We need HTTP service for provisioning.
  eventmgr::system_event_post(ZW_SYSTEM_EVENT_HTTPD_REINIT);
  ZW_WAIT_FOR_EVENTS(eventmgr::system_states(), ZW_SYSTEM_STATE_HTTPD_READY, /*reset=*/false,
                     /*all=*/true, HTTPD_STARTUP_TIMEOUT,
                     ESP_RETURN_ON_ERROR(_provision_do(config)), {
                       ESP_LOGW(TAG, "HTTP service init timeout");
                       return ESP_FAIL;
                     });

  // If we reach here, provisioning must have been successful
  states_.PROVISION = false;
  // We will re-init again to enter regular service mode.
  eventmgr::system_event_post(ZW_SYSTEM_EVENT_NET_REINIT);

  return ESP_OK;
}

esp_timer_handle_t station_reconnect_timer_;

void _station_retry_connect(TimerHandle_t) {
  if (esp_err_t __ret = esp_wifi_connect(); __ret != ESP_OK) {
    ESP_LOGW(TAG, "Station re-connect attempt failed, will retry...");
    esp_timer_start_once(station_reconnect_timer_, STATION_RECONNECT_BACKOFF * 1000 * 1000);
  }
}

void _station_connection_maint_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                       void* event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGD(TAG, "Station connection restored");
        eventmgr::system_states_set(ZW_SYSTEM_STATE_NET_STA_RECONNECT, false);
        break;

      case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGD(TAG, "Station disconnected, reason: %d", disconnected->reason);
        eventmgr::system_states_set(ZW_SYSTEM_STATE_NET_STA_RECONNECT);
        // Re-connect after some delay.
        esp_timer_start_once(station_reconnect_timer_, STATION_RECONNECT_BACKOFF * 1000 * 1000);
      } break;

      default:
        // Not interested in other events.
        break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP:
        _station_got_ip_event(*(ip_event_got_ip_t*)event_data);
        eventmgr::system_event_post(ZW_SYSTEM_EVENT_NET_STA_IP_REFRESH);
        break;

      default:
        // Not interested in other events.
        break;
    }
  }
}

esp_err_t _wifi_connect(const AppConfig::Wifi& config) {
  wifi_mode_t current_mode;
  ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&current_mode));

  if (config.ap.net_provision_only) {
    if (current_mode != WIFI_MODE_STA) ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA));
  } else {
    ESP_LOGD(TAG, "Configuring WiFi as AP + station...");
    if (current_mode != WIFI_MODE_APSTA) ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wifi_config = {};
    wifi_config.ap.max_connection = 5;
    strncpy((char*)wifi_config.ap.ssid, states_.APName.c_str(), sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = states_.APName.length();
    if (!config.ap.password.empty()) {
      ESP_LOGD(TAG, "AP is password protected...");
      strlcpy((char*)wifi_config.ap.password, config.ap.password.c_str(),
              sizeof(wifi_config.ap.password));
      wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
  }

  ESP_RETURN_ON_ERROR(_station_try_connect(config.station));
  if (eventmgr::system_states_peek(ZW_SYSTEM_STATE_NET_STA_IP_READY)) {
    // Prepare timer needed for reconnect with backoff
    esp_timer_create_args_t timer_conf = {
        .callback = ZWTimerWrapper<TAG, _station_retry_connect>,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "zw_wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_conf, &station_reconnect_timer_));
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   _station_connection_maint_handler, NULL));
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                   _station_connection_maint_handler, NULL));

    // Since we are now connected, reinitialize the HTTP service.
    eventmgr::system_event_post(ZW_SYSTEM_EVENT_HTTPD_REINIT);
  } else {
    // Otherwise, let's start provisioning
    states_.PROVISION = true;
    eventmgr::system_event_post(ZW_SYSTEM_EVENT_NET_REINIT);
  }

  return ESP_OK;
}

void _reconfigure(void*) {
  auto config = config::get()->wifi;
  if (states_.PROVISION) {
    ESP_GOTO_ON_ERROR(_wifi_provision(config), failed);
  } else {
    ESP_GOTO_ON_ERROR(_wifi_connect(config), failed);
  }
  return;

failed:
  eventmgr::SetSystemFailed();
}

void _network_task_event(int32_t event_id, void*, void*) {
  switch (event_id) {
    case ZW_SYSTEM_EVENT_NET_REINIT:
      if (xTaskCreate(ZWTaskWrapper<TAG, _reconfigure>, "zw_wifi_reconfigure", 3000, NULL, 5,
                      NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi reconfiguration worker!");
        eventmgr::SetSystemFailed();
      }
      break;
    default:
      ESP_LOGW(TAG, "Unrecognized event %d", event_id);
  }
}

esp_err_t _init_network_task(void) {
  return eventmgr::system_event_register_handler(
      ZW_SYSTEM_EVENT_NET_REINIT, eventmgr::SystemEventHandlerWrapper<TAG, _network_task_event>);
}

esp_err_t _wifi_init(void) {
  {
    ESP_LOGD(TAG, "WiFi initialization...");
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    if (wifi_config.nvs_enable) {
      // This framework stores WiFi configuration in file.
      ESP_LOGD(TAG, "WiFi NVS is disabled");
      wifi_config.nvs_enable = 0;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config));
  }

  // We have external config, no need to store in NVS.
  // Not needed, already disabled via `wifi_config.nvs_enable`.
  // ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  {
    auto config = config::get()->wifi;
    if (config.power_saving) {
      ESP_LOGD(TAG, "Enable power saving...");
      ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    }

    // Initialize sub-system state
    states_.flag_data = 0;
    states_.PROVISION = !config.station;
    states_.APName = _get_ap_name(config.ap);
  }

#ifdef ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS
  {
    // This must be configured *before* starting the AP.
    // Since there are several paths to start the AP, we do it here to cover all.
    dhcps_offer_t dhcps_dns_offer = 1;
    ESP_RETURN_ON_ERROR(tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_SET,
                                                   TCPIP_ADAPTER_DOMAIN_NAME_SERVER,
                                                   &dhcps_dns_offer, sizeof(dhcps_offer_t)));
  }
#endif

  // Bring up the station interface to set hostname
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_RETURN_ON_ERROR(esp_wifi_start());

  ESP_LOGI(TAG, "Setting host name: %s", states_.APName.c_str());
  ESP_RETURN_ON_ERROR(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, states_.APName.c_str()));

  return ESP_OK;
}

}  // namespace

extern const InternalStates& internal_states(void) { return states_; }

extern ip_addr_t StationIPAddr(void) { return states_.StationIPAddr; }
extern const std::string& Hostname(void) { return states_.APName; }

void ApplyStationConfig(void) {
  if (states_.PROVISION) {
    // We are already in provision mode
    // Simply configure the interface to try the new credential
    ESP_GOTO_ON_ERROR(esp_wifi_disconnect(), fallthrough);
    ESP_GOTO_ON_ERROR(_station_do_connect(config::get()->wifi.station), fallthrough);
    return;
  }

// If we are in regular service mode, things are a little more complicated.
// If we were to do a proper "try", we have permanent event handlers that
// need to unregister, also need to notify the entire system so components
// can react accordingly...
// But, we don't have to worry about it, because...
// "There is nothing a reboot cannot fix! (TM)"
fallthrough:
  eventmgr::system_event_post(ZW_SYSTEM_EVENT_REBOOT);
}

esp_err_t init(void) {
  ESP_LOGD(TAG, "Initializing...");
  ESP_RETURN_ON_ERROR(esp_netif_init());
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default());

  ESP_RETURN_ON_ERROR(_wifi_init());
  ESP_RETURN_ON_ERROR(_init_network_task());
  return ESP_OK;
}

void finit(void) {
  // Not much to do.
}

}  // namespace zw::esp8266::app::network
