
#ifndef APPTIME_INTERFACE
#define APPTIME_INTERFACE

#include <time.h>

#include "esp_err.h"

#include "ZWUtils.hpp"

namespace zw::esp8266::app::time {

// Refresh to the current configuration.
extern esp_err_t RefreshConfig(void);

extern utils::DataOrError<struct tm> ToLocalTime(time_t epoch_sec);
extern utils::DataOrError<struct tm> GetLocalTime(void);

}  // namespace zw::esp8266::app::time

#endif  // APPTIME_INTERFACE
