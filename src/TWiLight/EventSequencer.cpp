#include "EventSequencer.hpp"

#include <stdint.h>
#include <time.h>

#include <vector>
#include <deque>
#include <map>
#include <set>

#include "esp_log.h"

#include "ZWUtils.hpp"

#include "Interface.hpp"
#include "Interface_Private.hpp"

namespace zw::esp8266::app::twilight {
namespace {

inline constexpr char TAG[] = "TWiLight-EvtSeq";

void _process_time_range(int32_t start, int32_t window_end, int16_t event_idx,
                         const Config::Event::TimeRange& range, int32_t prev_offset,
                         std::vector<RawEventEntry>& raw_events) {
  int32_t end_second = -1;
  // If event already finished, it has no impact
  if (range.has_end) {
    end_second = (range.end + 1) * SECONDS_IN_A_MINUTE - 1;
    if (end_second < start) return;
  }

  const int32_t start_second = range.start * SECONDS_IN_A_MINUTE;
  if (start_second > start) {
    // Not yet started for today?
    if (start_second <= window_end) {
      // Only care about the event if it starts within the precomputed range
      raw_events.push_back({start_second, end_second, event_idx});
    }
    // End specified, must have finished from last cycle, therefore no impact
    if (range.has_end) return;
    // Started from last cycle without end time
    raw_events.push_back({start_second - prev_offset, end_second, event_idx});
  } else {
    // Already started and not finished
    raw_events.push_back({start_second, end_second, event_idx});
  }
}

}  // namespace

std::vector<RawEventEntry> generate_raw_events(const std::vector<Config::Event>& events,
                                               const struct tm& start_tm) {
  int32_t start = get_second_of_day(start_tm);
  int32_t window_end = start + PRECOMPUTE_WINDOW;

  std::vector<RawEventEntry> result;
  for (int16_t event_idx = 0; event_idx < events.size(); event_idx++) {
    const Config::Event& event = events[event_idx];
    switch (event.type) {
      case Config::Event::Type::RECURRENT_DAILY: {
        _process_time_range(start, window_end, event_idx, event.daily, SECONDS_IN_A_DAY, result);
      } break;

      case Config::Event::Type::RECURRENT_WEEKLY: {
        uint8_t day_mask = 1 << start_tm.tm_wday;
        bool on_day = event.weekly.days & day_mask;

        int prev_days = 0;
        while (++prev_days != 7) {
          day_mask = (day_mask == 1) ? (1 << 6) : (day_mask >> 1);
          if (event.weekly.days & day_mask) break;
        }
        _process_time_range(on_day ? start : -1, on_day ? window_end : -1, event_idx, event.weekly,
                            prev_days * SECONDS_IN_A_DAY, result);
      } break;

      case Config::Event::Type::RECURRENT_ANNUAL: {
        struct tm event_date_approx = start_tm;
        event_date_approx.tm_hour = 12;  // Set to mid-day to avoid boundary wrapping
        event_date_approx.tm_mday = event.annual.date.day;
        event_date_approx.tm_mon = event.annual.date.month_idx;
        event_date_approx.tm_year = start_tm.tm_year;

        if (mktime(&event_date_approx) < 0) {
          ESP_LOGW(TAG, "Failed to convert calendar time!");
          continue;
        }
        if (event_date_approx.tm_mday != event.annual.date.day) {
          // The event is on the leap day (Feb 29), but current year is not a leap year.
          continue;
        }
        bool on_day = start_tm.tm_yday == event_date_approx.tm_yday;
        int prev_days = start_tm.tm_yday - event_date_approx.tm_yday;
        if (prev_days <= 0) prev_days += 365;
        _process_time_range(on_day ? start : -1, on_day ? window_end : -1, event_idx, event.weekly,
                            prev_days * SECONDS_IN_A_DAY, result);
      } break;

      default:
        ESP_LOGW(TAG, "Unsupported event: %s", print_event(event).c_str());
    }
  }
  return result;
}

struct RawEventEntryCompare {
  bool operator()(const RawEventEntry* a, const RawEventEntry* b) const {
    return a->event_idx != b->event_idx && a->event_idx != EVENT_IDX_MANUAL_OVERRIDE &&
           (a->event_idx < b->event_idx || b->event_idx == EVENT_IDX_MANUAL_OVERRIDE);
  }
};

std::deque<EventEntry> convert_raw_events(std::vector<RawEventEntry>&& raw_events,
                                          int32_t second_of_day) {
  struct EventData {
    std::vector<const RawEventEntry*> starts;
    std::vector<const RawEventEntry*> ends;
  };
  std::map<int32_t, EventData> time_to_events;
  for (const RawEventEntry& event : raw_events) {
    time_to_events[event.start].starts.push_back(&event);
    if (event.end >= 0) time_to_events[event.end].ends.push_back(&event);
  }

  int16_t effective_event_idx = EVENT_IDX_UNCONFIGURED;
  int32_t last_event_end = -1;
  int32_t end_time = std::min(second_of_day + PRECOMPUTE_WINDOW, SECONDS_IN_A_DAY);
  std::set<const RawEventEntry*, RawEventEntryCompare> active_events;

  std::deque<EventEntry> result;
  ESP_LOGI(TAG, "--- Event Sequence From %s ---",
           print_time(second_of_day / SECONDS_IN_A_MINUTE).c_str());
  for (auto& [time, event_data] : time_to_events) {
    // Events without specific end-time ends when the next scheduled event starts
    bool scheduled_event_start = false;
    for (const RawEventEntry* event : event_data.starts) {
      if (event->event_idx != EVENT_IDX_MANUAL_OVERRIDE) {
        scheduled_event_start = true;
        break;
      }
    }
    if (scheduled_event_start) {
      for (auto it = active_events.begin(); it != active_events.end();) {
        if ((*it)->end < 0) {
          it = active_events.erase(it);
          continue;
        }
        ++it;
      }
    }
    active_events.insert(event_data.starts.begin(), event_data.starts.end());
    for (const RawEventEntry* event : event_data.ends) active_events.erase(event);

    if (time >= second_of_day) {
      if (time >= end_time) {
        result.push_back({.completion = end_time, .event_idx = effective_event_idx});
        ESP_LOGI(TAG, "%d. Event %d --> [%s]", result.size(), effective_event_idx,
                 print_time(end_time / SECONDS_IN_A_MINUTE).c_str());
        last_event_end = end_time;
        break;
      }
      result.push_back({.completion = time, .event_idx = effective_event_idx});
      ESP_LOGI(TAG, "%d. Event %d --> %s", result.size(), effective_event_idx,
               print_time(time / SECONDS_IN_A_MINUTE).c_str());
      last_event_end = time;
    }
    effective_event_idx =
        active_events.empty() ? EVENT_IDX_UNCONFIGURED : (*active_events.rbegin())->event_idx;
  }
  if (last_event_end < end_time) {
    result.push_back({.completion = end_time, .event_idx = effective_event_idx});
    ESP_LOGI(TAG, "%d. Event %d --> {%s}", result.size(), effective_event_idx,
             print_time(end_time / SECONDS_IN_A_MINUTE).c_str());
  }
  return result;
}

}  // namespace zw::esp8266::app::twilight