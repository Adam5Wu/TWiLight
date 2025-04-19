// Appliance event management

// Note that this header intentionally doesn't have `#ifndef *_H`
// or `pragma once`. This is because it is never intended to be
// included anywhere other than the `main` module. In other words,
// declaration in this file is internal.
// If the module offers features for external used, it will put
// them in the `Interface.h`.

#include "esp_err.h"

#include "Interface.hpp"

namespace zw::esp8266::app::eventmgr {

// Initialize app event management
esp_err_t init(void);

// Called before a controlled reboot.
void finit(void);

}  // namespace zw::esp8266::app::eventmgr