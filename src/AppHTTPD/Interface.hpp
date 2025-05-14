
#ifndef APPHTTPD_INTERFACE
#define APPHTTPD_INTERFACE

#include "cJSON.h"

#include "esp_http_server.h"

#include "ZWUtils.hpp"

namespace zw::esp8266::app::httpd {

extern httpd_handle_t handle(void);

using HandlerRegistrar = esp_err_t (*)(httpd_handle_t);

// Add an external handler registrar
// Usually called only once during module init.
extern void add_ext_handler_registrar(HandlerRegistrar registrar);

// Parse a url fragment pointing at the (supposedly) start of query string and return
// the value of a specific parameter.
// The `expect_len` optionally constraints the value length (excluding null-terminator).
// If non-zero, will return failure if the value is longer.
extern utils::DataOrError<std::string> query_parse_param(const char* query_frag, const char* name,
  size_t expect_len = 0);

// Send file data as respose in an HTTP handler
extern esp_err_t send_file(httpd_req_t* req, FILE* f, size_t size);

// Send serialized JSON data as respose in an HTTP handler
extern esp_err_t send_json(httpd_req_t* req, const cJSON* json);

// Receive and parse serialized JSON data from request body in an HTTP handler
extern esp_err_t receive_json(httpd_req_t* req, utils::AutoReleaseRes<cJSON*>& json);

}  // namespace zw::esp8266::app::httpd

#endif  // APPHTTPD_INTERFACE
