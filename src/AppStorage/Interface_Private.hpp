
#ifndef APPSTORAGE_INTERFACE_PRIVATE
#define APPSTORAGE_INTERFACE_PRIVATE

#include "esp_err.h"

namespace zw::esp8266::app::storage {

extern esp_err_t rtcmem_init(void);
extern void rtcmem_finit(void);

extern esp_err_t nvs_init(void);
extern void nvs_finit(void);

}  // namespace zw::esp8266::app::storage

#endif  // APPSTORAGE_INTERFACE_PRIVATE
