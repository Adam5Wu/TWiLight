
#ifndef APP_TWILIGHT_INTERFACE_PRIVATE
#define APP_TWILIGHT_INTERFACE_PRIVATE

#include "esp_err.h"
#include "cJSON.h"

#include "ZWUtils.hpp"

#include "Interface.hpp"

namespace zw::esp8266::app::twilight {

extern esp_err_t marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                                const Config& update);

extern esp_err_t parse_transition(const cJSON* json, Config::Transition& container,
                                  bool strict = false);

extern void log_transition(const Config::Transition& transition);

extern esp_err_t marshal_transition(utils::AutoReleaseRes<cJSON*>& container,
                                    const Config::Transition& base,
                                    const Config::Transition& update);

struct ConfigState {
  bool setup;
  Config config;
};

extern utils::DataOrError<ConfigState> GetConfigState(void);

extern utils::ESPErrorStatus Setup_Enter(void);
extern utils::ESPErrorStatus Setup_Exit(bool save_changes);

extern utils::ESPErrorStatus Setup_StripSize(size_t num_pixels);
extern utils::ESPErrorStatus Setup_TestTransition(const Config::Transition& transition);

}  // namespace zw::esp8266::app::twilight

#endif  // APP_TWILIGHT_INTERFACE_PRIVATE
