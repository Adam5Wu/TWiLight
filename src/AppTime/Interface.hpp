
#ifndef APPTIME_INTERFACE
#define APPTIME_INTERFACE

#include "esp_err.h"

namespace zw::esp8266::app::time {

// Refresh to the current configuration.
extern esp_err_t RefreshConfig(void);

}  // namespace zw::esp8266::app::time

#endif  // APPTIME_INTERFACE
