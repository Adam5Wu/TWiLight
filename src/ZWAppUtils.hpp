#ifndef ZWAPP_FACILITIES
#define ZWAPP_FACILITIES

#include "esp_log.h"
#include "esp_system.h"

#include "FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"

#include "AppEventMgr/Interface.hpp"

namespace zw::esp8266::app {

template <const char* TAG, void (*func)(TimerHandle_t)>
void ZWTimerWrapper(TimerHandle_t timer) {
  if (!eventmgr::IsSystemFailed()) {
    func(timer);
    ESP_LOGD(TAG, "-> Heap: %d; Stack: %d", esp_get_free_heap_size(),
             uxTaskGetStackHighWaterMark(NULL));
  }
}

template <const char* TAG, void (*func)(void*)>
void ZWTaskWrapper(void* param) {
  if (!eventmgr::IsSystemFailed()) {
    func(param);
    ESP_LOGD(TAG, "=> Heap: %d; Stack: %d", esp_get_free_heap_size(),
             uxTaskGetStackHighWaterMark(NULL));
  }
  vTaskDelete(xTaskGetCurrentTaskHandle());
}

}  // namespace zw::esp8266::app

#endif  // ZWAPP_FACILITIES