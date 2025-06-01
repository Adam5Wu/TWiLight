
#ifndef APP_TWILIGHT_INTERFACE_PRIVATE
#define APP_TWILIGHT_INTERFACE_PRIVATE

#include "esp_err.h"
#include "cJSON.h"

#include "ZWUtils.hpp"

#include "Interface.hpp"

namespace zw::esp8266::app::twilight {

extern esp_err_t marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                                const Config& update);

extern utils::DataOrError<Config::Transition> parse_transition(const cJSON* json, bool strict);

extern utils::DataOrError<Config::TransitionsMap> parse_transitions_map(const cJSON* json,
                                                                        bool strict);

extern utils::DataOrError<Config::EventsList> parse_events_list(const cJSON* json, bool strict);

struct ConfigState {
  bool setup;
  Config config;
};

extern utils::DataOrError<ConfigState> GetConfigState(void);

extern utils::ESPErrorStatus Setup_Enter(void);
extern utils::ESPErrorStatus Setup_Exit(bool save_changes);

extern utils::ESPErrorStatus Setup_StripSize(size_t num_pixels);
extern utils::ESPErrorStatus Setup_TestTransition(const Config::Transition& transition);

extern utils::ESPErrorStatus Set_Transitions(Config::TransitionsMap&& transitions);
extern utils::ESPErrorStatus Set_Events(Config::EventsList&& events);

}  // namespace zw::esp8266::app::twilight

#endif  // APP_TWILIGHT_INTERFACE_PRIVATE
