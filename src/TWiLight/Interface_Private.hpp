
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

extern std::string print_time(uint16_t time);
extern std::string print_event(const Config::Event& event);

struct ConfigState {
  bool setup;
  Config config;
};

extern utils::DataOrError<ConfigState> GetConfigState(void);

inline constexpr int32_t SECONDS_IN_A_MINUTE = 60;
inline constexpr int32_t SECONDS_IN_AN_HOUR = 60 * SECONDS_IN_A_MINUTE;
inline constexpr int32_t SECONDS_IN_A_DAY = 24 * SECONDS_IN_AN_HOUR;

inline constexpr int32_t PRECOMPUTE_WINDOW = 4 * SECONDS_IN_AN_HOUR;  // Look 4 hours ahead
static_assert(PRECOMPUTE_WINDOW < SECONDS_IN_A_DAY);

inline constexpr int16_t EVENT_IDX_UNCONFIGURED = -1;
inline constexpr int16_t EVENT_IDX_MANUAL_OVERRIDE = -2;
inline constexpr int16_t EVENT_IDX_UNINITIALIZED = -3;

struct EventEntry {
  int32_t completion;  // Completion time of this event
  int16_t event_idx;   // >=0 : Event index in `config.events`; otherwise, see `EVENT_IDX_`
};

extern utils::ESPErrorStatus Setup_Enter(void);
extern utils::ESPErrorStatus Setup_Exit(bool save_changes);

extern utils::ESPErrorStatus Setup_StripSize(size_t num_pixels);
extern utils::ESPErrorStatus Setup_TestTransition(Config::Transition&& transition);

extern utils::ESPErrorStatus Set_Transitions(Config::TransitionsMap&& transitions);
extern utils::ESPErrorStatus Set_Events(Config::EventsList&& events);

extern utils::ESPErrorStatus Perform_Override(int32_t duration, Config::Transition&& transition);

}  // namespace zw::esp8266::app::twilight

#endif  // APP_TWILIGHT_INTERFACE_PRIVATE
