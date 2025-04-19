// Captive DNS service
//
// A quick and dirty DNS "service" that redirects all
// DNS queries to the current AP's IP address.

namespace zw::esp8266::app::network {

// Task that performs the service
void captive_dns_task(void*);

}  // namespace zw::esp8266::app::network
