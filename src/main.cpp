#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_fast_boot.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "AppStorage/Module.hpp"
#include "AppConfig/Module.hpp"
#include "AppEventMgr/Module.hpp"
#include "AppNetwork/Module.hpp"
#include "AppTime/Module.hpp"
#include "AppHTTPD/Module.hpp"

namespace zw::esp8266::app {
namespace {

inline constexpr char TAG[] = "Main";

inline const char* _model_name(esp_chip_model_t model) {
  switch (model) {
    case CHIP_ESP8266:
      return "ESP8266";
    case CHIP_ESP32:
      return "ESP32";
  }
  return "Unknown Chip";
}

void _greeting(void) {
  ESP_LOGI(TAG, "=============================");
  ESP_LOGI(TAG, "ZWAppliance %s", ZW_APPLIANCE_NAME);
  {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "%s (%d core%s) @ %d Mhz", _model_name(chip_info.model), chip_info.cores,
             chip_info.cores > 1 ? "s" : "", CONFIG_ESP8266_DEFAULT_CPU_FREQ_MHZ);

    ESP_LOGI(TAG, "Silicon revision: %d", chip_info.revision);
    ESP_LOGI(TAG, "%s flash: %dKB",
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "Embedded" : "External",
             spi_flash_get_chip_size() / 1024);
  }
  ESP_LOGI(TAG, "SDK version: %s", esp_get_idf_version());
  ESP_LOGI(TAG, "Free heap memory: %d", esp_get_free_heap_size());
  ESP_LOGI(TAG, "=============================");
}

void _reboot_event(int32_t event_id, void* event_data, void* handler_arg) {
  // Give a little time for the signaler to complete their call.
  vTaskDelay(CONFIG_FREERTOS_HZ / 10);

  // Reverse order of initialization
  ESP_LOGI(TAG, "Finalizing components...");
  httpd::finit();
  network::finit();
  time::finit();
  config::finit();
  eventmgr::finit();
  storage::finit();

  ESP_LOGI(TAG, "Rebooting...");

  if (eventmgr::system_states_peek(ZW_SYSTEM_STATE_OTA_PENDING)) {
#ifdef ZW_APPLIANCE_FASTBOOT
    // Disable fast-boot since we will be booting from a new image.
    esp_fast_boot_disable();
#endif
    esp_restart();
  } else {
    // Note that this always works, even if bootloader fast boot is disabled.
    esp_fast_boot_restart();
  }
}

}  // namespace

esp_err_t main(void) {
  _greeting();

#ifdef ZW_APPLIANCE_FASTBOOT
  esp_fast_boot_enable();
#endif

  // Order independent sub-system init
  ESP_RETURN_ON_ERROR(storage::init());
  ESP_RETURN_ON_ERROR(eventmgr::init());

  // Order dependent sub-system initialization
  ESP_RETURN_ON_ERROR(config::init());
#ifdef ZW_SYSTIME_AVAILABLE
  ESP_RETURN_ON_ERROR(time::init());
#endif
  ESP_RETURN_ON_ERROR(network::init());
  ESP_RETURN_ON_ERROR(httpd::init());

  ESP_LOGD(TAG, "** Heap: %d; Stack: %d", esp_get_free_heap_size(),
           uxTaskGetStackHighWaterMark(NULL));

  // Adjust system state per config
  if (config::get()->dev_mode) {
    ESP_RETURN_ON_ERROR(storage::remount_system_rw());
  }

  // Initialization is now complete. And depending on the situation,
  // the appliance may either start regular serving or go into network
  // provisioning mode, and may cycle between them.
  // If anything goes wrong, the ZW_SYSTEM_STATE_FAILURE event will be signaled.

  ESP_RETURN_ON_ERROR(eventmgr::system_event_register_handler(
      ZW_SYSTEM_EVENT_REBOOT, eventmgr::SystemEventHandlerWrapper<TAG, _reboot_event>));

  // Send the bootstrap signal to start the system
  eventmgr::system_event_post(ZW_SYSTEM_EVENT_NET_REINIT);
  return ESP_OK;
}

}  // namespace zw::esp8266::app

using namespace zw::esp8266::app;

extern "C" void app_main(void) {
  ESP_GOTO_ON_ERROR(main(), failed);
  return;

failed:
  ESP_LOGE(TAG, "Appliance initialization failed!");
}