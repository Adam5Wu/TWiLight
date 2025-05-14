// Web Config Update handler

// Note that this header intentionally doesn't have `#ifndef *_H`
// or `pragma once`. This is because it is an internal unit to
// the local module, never intended to be included anywhere else.
// If the module offers features for external used, it will put
// them in the `Interface.h`.

#include "esp_http_server.h"

namespace zw::esp8266::app::httpd {

bool sysfunc_config(const char* feature, httpd_req_t* req);

}  // namespace zw::esp8266::app::httpd
