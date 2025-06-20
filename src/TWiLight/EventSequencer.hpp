// Event sequencer for TWiLight

// Note that this header intentionally doesn't have `#ifndef *_H`
// or `pragma once`. This is because it is an internal unit to
// the local module, never intended to be included anywhere else.
// If the module offers features for external used, it will put
// them in the `Interface.h`.

#include <stdint.h>
#include <time.h>

#include <vector>
#include <deque>

#include "Interface.hpp"
#include "Interface_Private.hpp"

namespace zw::esp8266::app::twilight {

inline int32_t get_second_of_day(const struct tm& time) {
  return time.tm_hour * SECONDS_IN_AN_HOUR + time.tm_min * SECONDS_IN_A_MINUTE + time.tm_sec;
}

struct RawEventEntry {
  int32_t start, end;
  int16_t event_idx;
};

// Given a list of events, and timing information, produce a raw sequence of events
// that may impact the state covering a predefined duration.
//
// Parameters:
//   events -- a list of configured events
//   start_tm -- the start time of interest
std::vector<RawEventEntry> generate_raw_events(const std::vector<Config::Event>& events,
                                               const struct tm& start_tm);

std::deque<EventEntry> convert_raw_events(std::vector<RawEventEntry>&& raw_events,
                                          int32_t second_of_day);

}  // namespace zw::esp8266::app::twilight