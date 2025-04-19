// Regular file serving handler

// Note that this header intentionally doesn't have `#ifndef *_H`
// or `pragma once`. This is because it is an internal unit to
// the local module, never intended to be included anywhere else.
// If the module offers features for external used, it will put
// them in the `Interface.h`.

#include "esp_err.h"

#include "esp_http_server.h"

namespace zw::esp8266::app::httpd {

// Register file serving handler
// This handler should be registered the last since it captures the root.
esp_err_t register_handler_fileserv(httpd_handle_t httpd);

}  // namespace zw::esp8266::app::httpd
