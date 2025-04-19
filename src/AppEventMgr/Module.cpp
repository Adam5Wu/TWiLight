#include "Module.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"

#include "Interface.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define SYSTEM_EVENT_POST_TIMEOUT (1 * CONFIG_FREERTOS_HZ)  // 1 sec

namespace zw::esp8266::app::eventmgr {
namespace {

inline constexpr char TAG[] = "EventMgr";

ESP_EVENT_DEFINE_BASE(ZW_SYSTEM_EVENTS);

volatile EventGroupHandle_t system_states_ = NULL;

}  // namespace

esp_err_t system_event_post(int32_t event_id, const void* event_data, size_t data_size) {
  return esp_event_post(ZW_SYSTEM_EVENTS, event_id, (void*)event_data, data_size,
                        SYSTEM_EVENT_POST_TIMEOUT);
}

esp_err_t system_event_register_handler(int32_t event_id, esp_event_handler_t event_handler,
                                        void* handler_arg) {
  return esp_event_handler_register(ZW_SYSTEM_EVENTS, event_id, event_handler, handler_arg);
}

esp_err_t system_event_unregister_handler(int32_t event_id, esp_event_handler_t event_handler) {
  return esp_event_handler_unregister(ZW_SYSTEM_EVENTS, event_id, event_handler);
}

EventGroupHandle_t system_states(void) { return system_states_; }

EventBits_t system_states_peek(EventBits_t states) {
  return xEventGroupGetBits(system_states_) & states;
}

EventBits_t system_states_set(EventBits_t states, bool set) {
  return (set ? xEventGroupSetBits(system_states_, states)
              : xEventGroupClearBits(system_states_, states)) &
         states;
}

void SetSystemFailed(void) {
  ESP_LOGE(TAG, "*** System failure ***");
  if (system_states_set(ZW_SYSTEM_STATE_FAILURE) == 0) {
    ESP_LOGE(TAG, "System failure signalled!");
  }
}

esp_err_t init(void) {
  system_states_ = xEventGroupCreate();
  if (!system_states_) {
    ESP_LOGE(TAG, "Failed to allocate system states group");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void finit(void) {
  // Not much to do.
}

}  // namespace zw::esp8266::app::eventmgr