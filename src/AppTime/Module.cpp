#include "Module.hpp"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "lwip/inet.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"
#include "ZWAppUtils.hpp"

#ifdef ZW_APPLIANCE_COMPONENT_TIME_SNTP
#include "lwip/apps/sntp.h"
#endif

#include "AppStorage/Interface.hpp"
#include "AppEventMgr/Interface.hpp"
#include "AppConfig/Interface.hpp"

#define TIME_RTC_TRACKING_CYCLE 5  // 5 sec

namespace zw::esp8266::app::time {
namespace {

inline constexpr char TAG[] = "Time";

using config::AppConfig;

struct {
#ifdef ZW_APPLIANCE_COMPONENT_TIME_RTC_TRACKING
  uint8_t rtc_tracking_cycle;
#endif
} states_;

struct BootRecord {
  // The wall-clock time at the last checkpoint
  struct timeval last_known;
};

storage::RTCData<BootRecord> rtc_data_;

std::string _print_time(const timeval& tv) {
  struct tm time_tm;
  if (localtime_r(&tv.tv_sec, &time_tm) == NULL) {
    return "(failed to convert)";
  }
  char time_str[30];
  if (strftime(time_str, 30, "%F %T %Z", &time_tm) == 0) {
    return "(failed to format)";
  }
  return time_str;
}

// This function will only adjust time forward
inline esp_err_t _rebase_time(const timeval& base) {
  struct timeval cur_time;
  if (gettimeofday(&cur_time, NULL) != 0) {
    ESP_LOGW(TAG, "Unable to get current time");
    return ESP_FAIL;
  }
  if (cur_time.tv_sec < base.tv_sec ||
      (cur_time.tv_sec == base.tv_sec && cur_time.tv_usec < base.tv_usec)) {
    return settimeofday(&base, NULL) != 0 ? ESP_FAIL : ESP_OK;
  }
  return ESP_OK;
}

esp_err_t _rebase_time_from_rtcmem(void) {
  // The RTC counter was always reset at boot, so there is no good way to find
  // out time elapsed since the last checkpoint (before reset / deep sleep).
  //
  // Although there is a small fraction of seconds (from boot until now) we could
  // account for, but IMO it isn't really worth the effort, because we already
  // lost track of a major bulk of time anyway...
  //
  // This is only best-effort, to get better time accuracy, enable NTP (or use
  // other time sync mechanisms).
  return _rebase_time(rtc_data_->last_known);
}

esp_err_t _rebase_time_from_string(const std::string& time_str) {
  if (time_str.empty()) {
    ESP_LOGW(TAG, "Empty time specifier");
    return ESP_ERR_NOT_FOUND;
  }

  struct tm base_tm = {};
  if (strptime(time_str.c_str(), "%F %T", &base_tm) == NULL) {
    ESP_LOGW(TAG, "Time specifier '%s' failed to parse", time_str.c_str());
    return ESP_ERR_INVALID_ARG;
  }

  timeval base{.tv_sec = mktime(&base_tm), .tv_usec = 0};
  if (base.tv_sec == (time_t)-1) {
    ESP_LOGW(TAG, "Time specifier '%s' failed to convert", time_str.c_str());
    return ESP_ERR_INVALID_ARG;
  }

  return _rebase_time(base);
}

esp_err_t _rebase_time_from_config(void) {
  auto config = config::get()->time;
  if (!config.baseline.empty()) {
    return _rebase_time_from_string(config.baseline);
  } else {
    ESP_LOGW(TAG, "Baseline time not configured.");
    return ESP_OK;
  }
}

esp_err_t _boot_rebase_time(void) {
  if (rtc_data_->last_known.tv_sec) {
    ESP_LOGD(TAG, "Restoring time from RTC...");
    ESP_RETURN_ON_ERROR(_rebase_time_from_rtcmem());
  } else {
    ESP_LOGD(TAG, "RTC stored time not available, checking config...");
    ESP_RETURN_ON_ERROR(_rebase_time_from_config());
  }
  return ESP_OK;
}

#ifdef ZW_APPLIANCE_COMPONENT_TIME_RTC_TRACKING

void _rtc_time_update(void) {
  ESP_LOGD(TAG, "Refreshing RTC memory...");
  BootRecord update;
  if (gettimeofday(&update.last_known, NULL) != 0) {
    ESP_LOGW(TAG, "Unable to get current time");
    return;
  }

  rtc_data_ = update;
}

void _rtc_time_tracker(void) {
  if (++states_.rtc_tracking_cycle < TIME_RTC_TRACKING_CYCLE) return;
  states_.rtc_tracking_cycle = 0;
  _rtc_time_update();
}

#endif  // ZW_APPLIANCE_COMPONENT_TIME_RTC_TRACKING

#ifdef ZW_APPLIANCE_COMPONENT_TIME_SNTP

std::string serving_ntp_server;

void _sntp_sync_event(struct timeval* tv, int64_t delta) {
  std::string time_str = _print_time(*tv);
  ESP_LOGI(TAG, "SNTP time: %s (delta %d.%03d sec)", time_str.c_str(), (int32_t)(delta / 1000000),
           abs((int32_t)(delta % 1000000)) / 1000);
  eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_TRACKING);
}

void _sntp_config(const std::string& ntp_server) {
  ESP_LOGD(TAG, "Starting NTP service...");
  // NTP name resolution is an async process, persist the name string.
  ESP_LOGI(TAG, "Using NTP server: %s", ntp_server.c_str());
  serving_ntp_server = ntp_server;

  // Try parse input as IPv4 string
  ip_addr_t server_addr;
  server_addr.addr = inet_addr(ntp_server.c_str());
  if (server_addr.addr != INADDR_NONE) {
    sntp_setserver(0, &server_addr);
  } else {
    sntp_setservername(0, serving_ntp_server.c_str());
  }
  sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
  sntp_set_time_sync_notification_cb(_sntp_sync_event);
  sntp_init();
}

#endif  // ZW_APPLIANCE_COMPONENT_TIME_SNTP

void _time_task(TimerHandle_t) {
  // Make smooth adjustment to the system clock.
  {
    uint64_t delta;
    {
      esp_irqflag_t flag;
      flag = soc_save_local_irq();
      adjust_boot_time(&delta);
      soc_restore_local_irq(flag);
    }
    ESP_LOGD(TAG, "Delta = %dms", (uint32_t)(delta / 1000));
    eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_ALIGNED, delta < 1000000);
  }

#ifdef ZW_APPLIANCE_COMPONENT_TIME_RTC_TRACKING
  _rtc_time_tracker();
#endif

  if (!eventmgr::system_states_peek(ZW_SYSTEM_STATE_TIME_NTP_DISABLED |
                                    ZW_SYSTEM_STATE_TIME_NTP_TRACKING)) {
    auto config = config::get()->time;
    if (config.ntp_server.empty()) {
      ESP_LOGD(TAG, "NTP service not configured");
      eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_DISABLED);
    } else {
#ifdef ZW_APPLIANCE_COMPONENT_TIME_SNTP
      if (eventmgr::system_states_peek(ZW_SYSTEM_STATE_NET_STA_IP_READY)) {
        if (!sntp_enabled()) _sntp_config(config.ntp_server);
      } else {
        ESP_LOGD(TAG, "NTP waiting for WiFi station connection...");
      }
#else
      ESP_LOGW(TAG, "NTP time sync was disabled in this build!");
      eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_DISABLED);
#endif  // ZW_APPLIANCE_COMPONENT_TIME_SNTP
    }
  }
}

esp_err_t _init_time_task(void) {
  ESP_LOGD(TAG, "Starting time task...");

  TimerHandle_t time_task_handle_;
  ESP_RETURN_ON_ERROR((time_task_handle_ = xTimerCreate("zw_time_maint", CONFIG_FREERTOS_HZ, pdTRUE,
                                                        NULL, ZWTimerWrapper<TAG, _time_task>),
                       time_task_handle_ != NULL)
                          ? ESP_OK
                          : ESP_FAIL);
  ZW_RETURN_ON_ERROR(
      (xTimerStart(time_task_handle_, CONFIG_FREERTOS_HZ) == pdPASS) ? ESP_OK : ESP_FAIL, {},
      xTimerDelete(time_task_handle_, CONFIG_FREERTOS_HZ));
  return ESP_OK;
}

void _log(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) {
    ESP_LOGW(TAG, "Unable to get current time");
    return;
  }
  std::string time_str = _print_time(tv);
  ESP_LOGI(TAG, "Current time: %s", time_str.c_str());
}

esp_err_t _set_timezone(const std::string& tz) {
  ESP_LOGD(TAG, "Setting timezone data: %s", tz.c_str());
  if (setenv("TZ", tz.c_str(), 1) != 0) {
    ESP_LOGW(TAG, "Failed to set timezone data");
    return ESP_FAIL;
  }
  tzset();
  _log();
  return ESP_OK;
}

esp_err_t _boot_settimezone(void) {
  auto config = config::get()->time;
  if (!config.timezone.empty()) {
    ESP_RETURN_ON_ERROR(_set_timezone(config.timezone));
  } else {
    ESP_LOGD(TAG, "Timezone is not configured");
    _log();
  }
  return ESP_OK;
}

esp_err_t _init_time() {
  rtc_data_ = storage::rtcmem_alloc<BootRecord>();
  if (!rtc_data_) {
    ESP_LOGE(TAG, "Failed to allocate RTC memory...");
    return ESP_ERR_NO_MEM;
  }
  ESP_RETURN_ON_ERROR(_boot_rebase_time());
  ESP_RETURN_ON_ERROR(_boot_settimezone());

// Initialize sub-system state
#ifdef ZW_APPLIANCE_COMPONENT_TIME_RTC_TRACKING
  states_.rtc_tracking_cycle = 0;
#endif

#ifdef ZW_APPLIANCE_COMPONENT_TIME_SMOOTH_LIMIT
  set_adjtime_correction_limit(ZW_APPLIANCE_COMPONENT_TIME_SMOOTH_LIMIT);
#endif

  return ESP_OK;
}

}  // namespace

esp_err_t RefreshConfig(void) {
  AppConfig::Time config = config::get()->time;

  // Apply baseline time update
  if (!config.baseline.empty()) {
    ESP_LOGD(TAG, "Applying baseline time...");
    ESP_RETURN_ON_ERROR(_rebase_time_from_string(config.baseline));
  }

  // Apply NTP server config
  if (config.ntp_server.empty()) {
    if (!serving_ntp_server.empty()) {
      serving_ntp_server.clear();
      ESP_LOGI(TAG, "Stopping NTP service...");
      sntp_stop();
      eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_DISABLED);
      eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_TRACKING, false);
    }
  } else {
    if (config.ntp_server != serving_ntp_server) {
      eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_TRACKING, false);
      eventmgr::system_states_set(ZW_SYSTEM_STATE_TIME_NTP_DISABLED, false);
      _sntp_config(config.ntp_server);
    }
  }

  // Apply timezone update
  if (config.timezone.empty()) {
    ESP_RETURN_ON_ERROR(_set_timezone("UTC0"));
  } else {
    if (config.timezone != getenv("TZ")) {
      ESP_RETURN_ON_ERROR(_set_timezone(config.timezone));
    }
  }

  return ESP_OK;
}

esp_err_t init(void) {
  ESP_LOGD(TAG, "Initializing...");
  ESP_RETURN_ON_ERROR(_init_time());
  ESP_RETURN_ON_ERROR(_init_time_task());
  return ESP_OK;
}

void finit(void) {
  // Store the latest time to minimize time loss across reboot.
  _rtc_time_update();
}

}  // namespace zw::esp8266::app::time
