
#ifndef APPHTTPD_INTERFACE_PRIVATE
#define APPHTTPD_INTERFACE_PRIVATE

#include "AppConfig/Interface.hpp"

#include "esp_err.h"
#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

namespace zw::esp8266::app::httpd {

struct ServingConfig {
  config::AppConfig::HttpServer httpd;
  bool dav_enabled;
  bool provisioning;
};

extern const ServingConfig& serving_config(void);

// Parse a url fragment pointing at the (supposedly) start of query string and return
// the value of a specific parameter.
// The `expect_len` optionally constraints the value length (excluding null-terminator).
// If non-zero, will return failure if the value is longer.
extern utils::DataOrError<std::string> query_parse_param(const char* query_frag, const char* name,
                                                         size_t expect_len);

// Send file data as respose in an HTTP handler
extern esp_err_t send_file(httpd_req_t* req, FILE* f, size_t size);

}  // namespace zw::esp8266::app::httpd

#endif  // APPHTTPD_INTERFACE_PRIVATE
