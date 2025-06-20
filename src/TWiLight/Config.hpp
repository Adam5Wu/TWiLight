// Config handling for TWiLight

// Note that this header intentionally doesn't have `#ifndef *_H`
// or `pragma once`. This is because it is an internal unit to
// the local module, never intended to be included anywhere else.
// If the module offers features for external used, it will put
// them in the `Interface.h`.

#include "esp_err.h"

#include "cJSON.h"

#include "ZWUtils.hpp"
#include "Interface.hpp"

namespace zw::esp8266::app::twilight {

esp_err_t parse_config(const cJSON* json, Config& container, bool strict);

void log_config(const Config& config);

esp_err_t marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                         const Config& update);

std::string print_transition(const Config::Transition& transition);


}  // namespace zw::esp8266::app::twilight
