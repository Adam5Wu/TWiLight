#include "Module.hpp"

#include <string>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"
#include "ZWAppUtils.hpp"

#include "Interface.hpp"
#include "Interface_Private.hpp"
#include "Handler_FileServ.hpp"

#ifdef ZW_APPLIANCE_COMPONENT_WEBDAV
#include "Handler_WebDAV.hpp"
#endif

#ifdef ZW_APPLIANCE_COMPONENT_WEB_SYSFUNC
#include "Handler_SysFunc.hpp"
#endif

#include "AppConfig/Interface.hpp"
#include "AppEventMgr/Interface.hpp"


#define HTTP_SERV_THREAD_WITH_DAV_STACK 5120
#define HTTP_SERV_THREAD_NO_DAV_STACK 3840

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD";

using config::AppConfig;

volatile httpd_handle_t httpd_ = NULL;

ServingConfig serving_config_;

inline esp_err_t _cleanup() {
  if (httpd_) {
    ESP_LOGD(TAG, "Stopping server...");
    ESP_RETURN_ON_ERROR(httpd_stop(httpd_));
    httpd_ = NULL;
  }
  return ESP_OK;
}

void _snapshot_serving_config() {
  {
    auto xconfig = config::get();
    serving_config_.httpd = xconfig->http_server;
    serving_config_.dav_enabled = xconfig->dev_mode.web_dav;
  }
  // If Wifi station is not ready, we need provisioning
  serving_config_.provisioning = !eventmgr::system_states_peek(ZW_SYSTEM_STATE_NET_STA_IP_READY);
}

inline bool _is_service_needed() {
  bool result = false;

  // Network provision needed
  if (serving_config_.provisioning) {
    ESP_LOGI(TAG, "Service needed by network provisioning");
    result = true;
  }
  // WebDAV for development
  if (serving_config_.dav_enabled) {
    ESP_LOGI(TAG, "Service needed by dev-mode WebDAV");
    result = true;
  }
  // Regular HTTP serving configured
  if (serving_config_.httpd) {
    ESP_LOGI(TAG, "Service configured for regular serving");
    result = true;
  }

  return result;
}

std::vector<HandlerRegistrar> ext_handler_registrar_list_;

inline esp_err_t _register_handlers() {
  if (serving_config_.dav_enabled) {
#ifdef ZW_APPLIANCE_COMPONENT_WEBDAV
    ESP_RETURN_ON_ERROR(register_handler_webdav(httpd_));
#else
    ESP_LOGW(TAG, "WebDAV is disabled in this build!");
#endif
  }

#ifdef ZW_APPLIANCE_COMPONENT_WEB_SYSFUNC
  ESP_RETURN_ON_ERROR(register_handler_sysfunc(httpd_));
#else
  ESP_LOGW(TAG, "Web-based system management is disabled in this build!");
#endif

  // Register external handlers
  for (auto& registrar : ext_handler_registrar_list_) {
    ESP_RETURN_ON_ERROR(registrar(httpd_));
  }

  // Ensure the captive handler is registered the last.
  if (serving_config_.httpd) {
    ESP_RETURN_ON_ERROR(register_handler_fileserv(httpd_));
  }
  return ESP_OK;
}

void _reconfigure(void*) {
  ESP_GOTO_ON_ERROR(_cleanup(), failed);
  {
    _snapshot_serving_config();

    if (_is_service_needed()) {
      ESP_LOGD(TAG, "Initializing service...");
      httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
      httpd_config.uri_match_fn = &httpd_uri_match_wildcard;
      httpd_config.stack_size = serving_config_.dav_enabled ? HTTP_SERV_THREAD_WITH_DAV_STACK
                                                            : HTTP_SERV_THREAD_NO_DAV_STACK;
      httpd_config.max_open_sockets = 10;
      httpd_config.keep_alive_enable = true;
      ESP_GOTO_ON_ERROR(httpd_start((httpd_handle_t*)&httpd_, &httpd_config), failed);

      // Populate additional custom data here.
      ESP_GOTO_ON_ERROR(_register_handlers(), failed);

      ESP_LOGD(TAG, "Service started");
      eventmgr::system_states_set(ZW_SYSTEM_STATE_HTTPD_READY);
      eventmgr::system_event_post(ZW_SYSTEM_EVENT_HTTPD_READY);
    } else {
      ESP_LOGD(TAG, "Service disabled");
    }
  }
  return;

failed:
  eventmgr::SetSystemFailed();
}

void _httpd_task_event(int32_t event_id, void*, void*) {
  switch (event_id) {
    case ZW_SYSTEM_EVENT_HTTPD_REINIT:
      if (xTaskCreate(ZWTaskWrapper<TAG, _reconfigure>, "zw_httpd_reconfigure", 1500, NULL, 5,
                      NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HTTPD reconfiguration worker!");
        eventmgr::SetSystemFailed();
      }
      break;
    default:
      ESP_LOGW(TAG, "Unrecognized event %d", event_id);
  }
}

esp_err_t _init_httpd_task(void) {
  return eventmgr::system_event_register_handler(
      ZW_SYSTEM_EVENT_HTTPD_REINIT, eventmgr::SystemEventHandlerWrapper<TAG, _httpd_task_event>);
}

}  // namespace

void add_ext_handler_registrar(HandlerRegistrar registrar) {
  ext_handler_registrar_list_.push_back(registrar);
}

httpd_handle_t handle(void) { return httpd_; }
const ServingConfig& serving_config(void) { return serving_config_; }

esp_err_t init(void) {
  ESP_LOGD(TAG, "Initializing...");
  ESP_RETURN_ON_ERROR(_init_httpd_task());
  return ESP_OK;
}

void finit(void) {
  // Not much to do.
}

}  // namespace zw::esp8266::app::httpd
