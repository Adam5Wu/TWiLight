
#ifndef APPEVENTMGR_INTERFACE
#define APPEVENTMGR_INTERFACE

#include <string>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event_base.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace zw::esp8266::app::eventmgr {

// ---------------------------
// System states management
#define ZW_SYSTEM_STATE_FAILURE BIT0
#define ZW_SYSTEM_STATE_RESTART BIT1
#define ZW_SYSTEM_STATE_BOOT_IMAGE_ALT BIT2
#define ZW_SYSTEM_STATE_TIME_NTP_DISABLED BIT3
#define ZW_SYSTEM_STATE_TIME_NTP_TRACKING BIT4
#define ZW_SYSTEM_STATE_TIME_ALIGNED BIT5
#define ZW_SYSTEM_STATE_NET_STA_IP_READY BIT6
#define ZW_SYSTEM_STATE_NET_STA_RECONNECT BIT7
#define ZW_SYSTEM_STATE_HTTPD_READY BIT8

extern EventGroupHandle_t system_states(void);

extern EventBits_t system_states_peek(EventBits_t states);
extern EventBits_t system_states_set(EventBits_t states, bool set = true);

inline bool IsSystemFailed(void) { return system_states_peek(ZW_SYSTEM_STATE_FAILURE); }
extern void SetSystemFailed(void);

// ---------------------------
// System events
// ESP_EVENT_DECLARE_BASE(ZW_SYSTEM_EVENTS);

#define ZW_SYSTEM_EVENT_REBOOT 0
#define ZW_SYSTEM_EVENT_NET_REINIT 1
#define ZW_SYSTEM_EVENT_HTTPD_REINIT 2
#define ZW_SYSTEM_EVENT_HTTPD_READY 3
#define ZW_SYSTEM_EVENT_NET_STA_IP_READY 4
#define ZW_SYSTEM_EVENT_NET_STA_IP_REFRESH 5
#define ZW_SYSTEM_EVENT_BOOT_IMAGE_ALT 6

extern esp_err_t system_event_post(int32_t event_id, const void* event_data, size_t data_size);

inline esp_err_t system_event_post(int32_t event_id) {
  return system_event_post(event_id, nullptr, 0);
}
inline esp_err_t system_event_post(int32_t event_id, const std::string& data) {
  return system_event_post(event_id, data.data(), data.length());
}

typedef void (*SystemEventHandler)(int32_t event_id, void* event_data, void* handler_arg);

template <const char* TAG, SystemEventHandler handler>
void SystemEventHandlerWrapper(void* handler_arg, esp_event_base_t event_base, int32_t event_id,
                               void* event_data) {
  if (!IsSystemFailed()) {
    handler(event_id, event_data, handler_arg);
    ESP_LOGD(TAG, "~> Heap: %d; Stack: %d", esp_get_free_heap_size(),
             uxTaskGetStackHighWaterMark(NULL));
  }
}

extern esp_err_t system_event_register_handler(int32_t event_id, esp_event_handler_t event_handler,
                                               void* handler_arg = nullptr);
extern esp_err_t system_event_unregister_handler(int32_t event_id,
                                                 esp_event_handler_t event_handler);

}  // namespace zw::esp8266::app::eventmgr

#endif  // APPEVENTMGR_INTERFACE
