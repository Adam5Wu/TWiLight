// TWiLight app module

// Note that this header intentionally doesn't have `#ifndef *_H`
// or `pragma once`. This is because it is never intended to be
// included anywhere other than the `main` module. In other words,
// declaration in this file is internal.
// If the module offers features for external used, it will put
// them in the `Interface.h`.

#include "esp_err.h"

namespace zw::esp8266::app::twilight {

// Initialize the TWiLight module's configuration.
// Note: This is *a lot* before most parts of the appliance sub-system
// are ready, so only do the bare minimum for registering config handlers.
esp_err_t config_init(void);

// Initialize the TWiLight module.
esp_err_t init(void);

// Called before a controlled reboot.
void finit(void);

}  // namespace zw::esp8266::app::twilight
