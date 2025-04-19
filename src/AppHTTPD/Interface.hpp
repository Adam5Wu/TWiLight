
#ifndef APPHTTPD_INTERFACE
#define APPHTTPD_INTERFACE

#include "esp_http_server.h"

namespace zw::esp8266::app::httpd {

extern httpd_handle_t handle(void);

}  // namespace zw::esp8266::app::httpd

#endif  // APPHTTPD_INTERFACE
