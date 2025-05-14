
#ifndef APPHTTPD_INTERFACE_PRIVATE
#define APPHTTPD_INTERFACE_PRIVATE

#include "AppConfig/Interface.hpp"

namespace zw::esp8266::app::httpd {

struct ServingConfig {
  config::AppConfig::HttpServer httpd;
  bool dav_enabled;
  bool provisioning;
};

extern const ServingConfig& serving_config(void);

}  // namespace zw::esp8266::app::httpd

#endif  // APPHTTPD_INTERFACE_PRIVATE
