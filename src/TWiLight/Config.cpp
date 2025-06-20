#include "Config.hpp"

#include "esp_log.h"

#include "cJSON.h"

#include "ZWUtils.hpp"

#include "LSPixel.hpp"

#include "AppConfig/Interface.hpp"
#include "Interface.hpp"

namespace zw::esp8266::app::twilight {
namespace {

inline constexpr char TAG[] = "TWiLight-Config";

namespace LS = ::zw::esp8266::lightshow;

//-------------------------
// Config handling macros

#define PARSE_AND_ASSIGN_FIELD(json, container, field, parse_func, strict) \
  ESP_RETURN_ON_ERROR(                                                     \
      config::parse_and_assign_field(json, #field, container.field, parse_func, strict))

#define PARSE_FIELD_INPLACE(json, container, field, parse_func, strict) \
  ESP_RETURN_ON_ERROR(parse_func(cJSON_GetObjectItem(json, #field), container.field, strict))

#define DIFF_AND_MARSHAL_FIELD(container, base, update, field, marshal_func) \
  ESP_RETURN_ON_ERROR(                                                       \
      config::diff_and_marshal_field(container, #field, base.field, update.field, marshal_func))

//----------------------
// Transition Type

static const std::unordered_map<std::string_view, Config::Transition::Type>
    transition_name_to_type_ = {
        {"uniform-color", Config::Transition::Type::UNIFORM_COLOR},
        {"color-wipe", Config::Transition::Type::COLOR_WIPE},
        {"color-wheel", Config::Transition::Type::COLOR_WHEEL},
};

utils::DataOrError<Config::Transition::Type> _decode_transition_type(const char* str) {
  return config::decode_enum<Config::Transition::Type, transition_name_to_type_>(str);
}

static const std::unordered_map<Config::Transition::Type, std::string_view>
    transition_type_to_name_ = {
        {Config::Transition::Type::UNIFORM_COLOR, "uniform-color"},
        {Config::Transition::Type::COLOR_WIPE, "color-wipe"},
        {Config::Transition::Type::COLOR_WHEEL, "color-wheel"},
};

std::string _encode_transition_type(const Config::Transition::Type& type) {
  return config::encode_enum<Config::Transition::Type, transition_type_to_name_>(type);
}

//----------------------
// Event Type

static const std::unordered_map<std::string_view, Config::Event::Type> event_name_to_type_ = {
    {"daily", Config::Event::Type::RECURRENT_DAILY},
    {"weekly", Config::Event::Type::RECURRENT_WEEKLY},
    {"annual", Config::Event::Type::RECURRENT_ANNUAL},
};

utils::DataOrError<Config::Event::Type> _decode_event_type(const char* str) {
  return config::decode_enum<Config::Event::Type, event_name_to_type_>(str);
}

static const std::unordered_map<Config::Event::Type, std::string_view> event_type_to_name_ = {
    {Config::Event::Type::RECURRENT_DAILY, "daily"},
    {Config::Event::Type::RECURRENT_WEEKLY, "weekly"},
    {Config::Event::Type::RECURRENT_ANNUAL, "annual"},
};

std::string _encode_event_type(const Config::Event::Type& type) {
  return config::encode_enum<Config::Event::Type, event_type_to_name_>(type);
}

//----------------------
// Color

utils::DataOrError<LS::RGB888> _decode_RGB8BHex(const char* str) {
  if (*str++ != '#') return ESP_ERR_INVALID_ARG;

  LS::RGB888 color;
  ASSIGN_OR_RETURN(color.r, utils::ParseHexByte(str));
  ASSIGN_OR_RETURN(color.g, utils::ParseHexByte(str + 2));
  ASSIGN_OR_RETURN(color.b, utils::ParseHexByte(str + 4));
  return color;
}

std::string _encode_RGB8BHex(const LS::RGB888& color) {
  return utils::DataBuf(8).PrintTo("#%02x%02x%02x", color.r, color.g, color.b);
}

//----------------------
// Time range

utils::DataOrError<Config::Event::TimeRange> _timerange_parser(cJSON* item) {
  Config::Event::TimeRange range = {};
  if (!cJSON_IsArray(item)) {
    ESP_LOGD(TAG, "Time range not an array");
    return ESP_ERR_INVALID_ARG;
  }
  switch (cJSON_GetArraySize(item)) {
    case 2: {
      range.has_end = true;
      ASSIGN_OR_RETURN(range.end, (config::string_decoder<uint16_t, config::decode_short_size>(
                                      cJSON_GetArrayItem(item, 1))));
    }
      [[fallthrough]];
    case 1: {
      ASSIGN_OR_RETURN(range.start, (config::string_decoder<uint16_t, config::decode_short_size>(
                                        cJSON_GetArrayItem(item, 0))));
    } break;

    default:
      ESP_LOGD(TAG, "Time range size unexpected");
      return ESP_ERR_INVALID_ARG;
  }
  return range;
}

std::string _print_time(uint16_t time) {
  if (time > 24 * 60) return "(invalid time)";
  return utils::DataBuf(6).PrintTo("%02d:%02d", time / 60, time % 60);
}

std::string _print_time_range(const Config::Event::TimeRange& range) {
  return range.has_end ? utils::DataBuf(20).PrintTo("%s - %s", _print_time(range.start).c_str(),
                                                    _print_time(range.end).c_str())
                       : utils::DataBuf(20).PrintTo("after %s", _print_time(range.start).c_str());
}

utils::DataOrError<cJSON*> _marshal_timerange(const Config::Event::TimeRange& base,
                                              const Config::Event::TimeRange& update) {
  if (base.start == update.start && base.end == update.end && base.has_end == update.has_end)
    return nullptr;

  utils::AutoReleaseRes<cJSON*> container(cJSON_CreateArray(), cJSON_Delete);
  if (!*container) return ESP_ERR_NO_MEM;

  ASSIGN_OR_RETURN(cJSON * start_time, (config::string_encoder<uint16_t, config::encode_short_size>(
                                           base.start, update.start)));
  if (!start_time) return ESP_ERR_NO_MEM;
  cJSON_AddItemToArray(*container, start_time);

  if (update.has_end) {
    ASSIGN_OR_RETURN(cJSON * end_time, (config::string_encoder<uint16_t, config::encode_short_size>(
                                           base.end, update.end)));
    if (!end_time) return ESP_ERR_NO_MEM;
    cJSON_AddItemToArray(*container, end_time);
  }

  return container.Drop();
}

//----------------------
// Weekdays

utils::DataOrError<uint8_t> _weekdays_parser(cJSON* item) {
  uint8_t days = 0;
  if (!cJSON_IsArray(item)) {
    ESP_LOGD(TAG, "Weekdays not an array");
    return ESP_ERR_INVALID_ARG;
  }
  for (int i = 0; i < cJSON_GetArraySize(item); ++i) {
    ASSIGN_OR_RETURN(
        uint8_t weekday_idx,
        (config::string_decoder<uint8_t, config::decode_byte_size>(cJSON_GetArrayItem(item, i))));
    if (weekday_idx >= 7) {
      ESP_LOGD(TAG, "Invalid weekday index");
      return ESP_ERR_INVALID_ARG;
    }
    days |= 1 << weekday_idx;
  }
  return days;
}

std::string _print_weekdays(uint8_t days) {
  std::string result;
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::SUNDAY)) result += "Sun,";
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::MONDAY)) result += "Mon,";
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::TUESDAY)) result += "Tue,";
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::WEDNESDAY)) result += "Wed,";
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::THURSDAY)) result += "Thu,";
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::FRIDAY)) result += "Fri,";
  if (days & static_cast<uint8_t>(Config::Event::Weekly::Day::SATURDAY)) result += "Sat,";
  if (!result.empty()) result.pop_back();
  return result;
}

utils::DataOrError<cJSON*> _marshal_weekdays(const uint8_t& base, const uint8_t& update) {
  if (base == update) return nullptr;

  utils::AutoReleaseRes<cJSON*> container(cJSON_CreateArray(), cJSON_Delete);
  if (!*container) return ESP_ERR_NO_MEM;

  for (uint8_t i = 0; i < 7; ++i) {
    if (update & (1 << i)) {
      cJSON* weekday = cJSON_CreateString(std::to_string(i).c_str());
      if (!weekday) return ESP_ERR_NO_MEM;

      cJSON_AddItemToArray(*container, weekday);
    }
  }
  return container.Drop();
}

//----------------------
// Day of year

utils::DataOrError<Config::Event::DayOfYear> _dayofyear_parser(cJSON* item) {
  static constexpr uint8_t max_days_in_month[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  Config::Event::DayOfYear date;
  if (!cJSON_IsArray(item)) {
    ESP_LOGD(TAG, "Day of year not an array");
    return ESP_ERR_INVALID_ARG;
  }
  if (cJSON_GetArraySize(item) != 2) {
    ESP_LOGD(TAG, "Day of year size unexpected");
    return ESP_ERR_INVALID_ARG;
  }

  ASSIGN_OR_RETURN(
      date.month_idx,
      (config::string_decoder<uint8_t, config::decode_byte_size>(cJSON_GetArrayItem(item, 0))));
  if (date.month_idx >= 12) {
    ESP_LOGD(TAG, "Invalid month index");
    return ESP_ERR_INVALID_ARG;
  }

  ASSIGN_OR_RETURN(
      date.day,
      (config::string_decoder<uint8_t, config::decode_byte_size>(cJSON_GetArrayItem(item, 1))));
  if (date.day > max_days_in_month[date.month_idx]) {
    ESP_LOGD(TAG, "Invalid day of month");
    return ESP_ERR_INVALID_ARG;
  }
  return date;
}

std::string _print_dayofyear(const Config::Event::DayOfYear& date) {
  static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return utils::DataBuf(20).PrintTo("%s %d", months[date.month_idx], date.day);
}

utils::DataOrError<cJSON*> _marshal_dayofyear(const Config::Event::DayOfYear& base,
                                              const Config::Event::DayOfYear& update) {
  if (base.month_idx == update.month_idx && base.day == update.day) return nullptr;

  utils::AutoReleaseRes<cJSON*> container(cJSON_CreateArray(), cJSON_Delete);
  if (!*container) return ESP_ERR_NO_MEM;

  ASSIGN_OR_RETURN(cJSON * month_idx, (config::string_encoder<uint8_t, config::encode_byte_size>(
                                          base.month_idx, update.month_idx)));
  if (!month_idx) return ESP_ERR_NO_MEM;
  cJSON_AddItemToArray(*container, month_idx);
  ASSIGN_OR_RETURN(cJSON * day, (config::string_encoder<uint8_t, config::encode_byte_size>(
                                    base.day, update.day)));
  if (!day) return ESP_ERR_NO_MEM;
  cJSON_AddItemToArray(*container, day);
  return container.Drop();
}

//----------------------
// Transition params

esp_err_t _parse_transition_uniform_color(const cJSON* json,
                                          Config::Transition::UniformColor& container,
                                          bool strict) {
  PARSE_AND_ASSIGN_FIELD(json, container, color,
                         (config::string_decoder<LS::RGB888, _decode_RGB8BHex>), strict);
  return ESP_OK;
}

esp_err_t _marshal_transition_uniform_color(utils::AutoReleaseRes<cJSON*>& container,
                                            const Config::Transition::UniformColor& base,
                                            const Config::Transition::UniformColor& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, color,
                         (config::string_encoder<LS::RGB888, _encode_RGB8BHex>));

  return ESP_OK;
}

esp_err_t _parse_transition(const cJSON* json, Config::Transition& container, bool strict) {
  PARSE_AND_ASSIGN_FIELD(json, container, duration_ms,
                         (config::string_decoder<size_t, config::decode_size>), strict);
  PARSE_AND_ASSIGN_FIELD(
      json, container, type,
      (config::string_decoder<Config::Transition::Type, _decode_transition_type>), strict);

  switch (container.type) {
    case Config::Transition::Type::UNIFORM_COLOR: {
      ESP_RETURN_ON_ERROR(_parse_transition_uniform_color(json, container.uniform_color, strict));
    } break;

    case Config::Transition::Type::COLOR_WIPE: {
      ESP_RETURN_ON_ERROR(ESP_ERR_NOT_SUPPORTED);
    } break;

    case Config::Transition::Type::COLOR_WHEEL: {
      ESP_RETURN_ON_ERROR(ESP_ERR_NOT_SUPPORTED);
    } break;

    default:
      return ESP_ERR_NOT_SUPPORTED;
  }

  return ESP_OK;
}

std::string _print_transition(const Config::Transition& transition) {
  if (!transition) return "(invalid transition)";

  std::string result;
  utils::DataBuf PrintBuf(64);
  result.append(_encode_transition_type(transition.type)).append(" | ");
  result.append(PrintBuf.PrintTo("%dms", transition.duration_ms));
  switch (transition.type) {
    case Config::Transition::Type::UNIFORM_COLOR: {
      result.append(
          PrintBuf.PrintTo(", %s", LS::to_string(transition.uniform_color.color).c_str()));
      break;
    }
    case Config::Transition::Type::COLOR_WIPE: {
      result.append(
          PrintBuf.PrintTo(", %s", LS::to_string(transition.uniform_color.color).c_str()));
      result.append(", <-->");
      result.append(", 100%W");
      break;
    }
    case Config::Transition::Type::COLOR_WHEEL: {
      result.append(", 100deg");
      result.append(", 100%W");
      result.append(", 100%I");
      break;
    }
    default:
      result.append("(unexpected transition)");
  }
  return result;
}

esp_err_t _marshal_transition(utils::AutoReleaseRes<cJSON*>& container,
                              const Config::Transition& base, const Config::Transition& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, duration_ms,
                         (config::string_encoder<size_t, config::encode_size>));
  DIFF_AND_MARSHAL_FIELD(
      container, base, update, type,
      (config::string_encoder<Config::Transition::Type, _encode_transition_type>));

  switch (update.type) {
    case Config::Transition::Type::UNIFORM_COLOR: {
      ESP_RETURN_ON_ERROR(
          _marshal_transition_uniform_color(container, base.uniform_color, update.uniform_color));
    } break;

    case Config::Transition::Type::COLOR_WIPE: {
      ESP_RETURN_ON_ERROR(ESP_ERR_NOT_SUPPORTED);
    } break;

    case Config::Transition::Type::COLOR_WHEEL: {
      ESP_RETURN_ON_ERROR(ESP_ERR_NOT_SUPPORTED);
    } break;

    default:
      return ESP_ERR_NOT_SUPPORTED;
  }

  return ESP_OK;
}

//----------------------
// Transitions map

esp_err_t _parse_transitions_map(const cJSON* json, Config::TransitionsMap& transitions,
                                 bool strict) {
  if (!cJSON_IsObject(json)) {
    ESP_LOGD(TAG, "Transitions not an object");
    if (strict) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
  }

  cJSON* entry;
  cJSON_ArrayForEach(entry, json) {
    if (!cJSON_IsObject(entry)) {
      if (cJSON_IsNull(entry)) {
        // This is an annotated deleted entry
        transitions.erase(entry->string);
        continue;
      }
      ESP_LOGD(TAG, "Transition not an object");
      if (strict) return ESP_ERR_INVALID_ARG;
      continue;
    }
    Config::Transition* transition;
    if (auto iter = transitions.find(entry->string); iter != transitions.end()) {
      transition = &iter->second;
    } else {
      transition = &transitions[entry->string];
    }
    ESP_RETURN_ON_ERROR(_parse_transition(entry, *transition, strict));
  }
  return ESP_OK;
}

utils::DataOrError<cJSON*> _marshal_transitions_map(const Config::TransitionsMap& base,
                                                    const Config::TransitionsMap& update) {
  utils::AutoReleaseRes<cJSON*> container(cJSON_CreateObject(), cJSON_Delete);
  if (!*container) return ESP_ERR_NO_MEM;

  // Store new and updated transitions
  for (const auto& [name, transition] : update) {
    utils::AutoReleaseRes<cJSON*> transition_elem;
    auto iter = base.find(name);
    if (iter == base.end()) {
      ESP_RETURN_ON_ERROR(_marshal_transition(transition_elem, {}, transition));
    } else {
      ESP_RETURN_ON_ERROR(_marshal_transition(transition_elem, iter->second, transition));
    }
    if (*transition_elem) {
      cJSON_AddItemToObject(*container, name.c_str(), transition_elem.Drop());
    }
  }
  // Annotate deleted transitions
  for (const auto& [name, transition] : base) {
    if (update.find(name) == update.end()) {
      cJSON* null_elem = cJSON_CreateNull();
      if (!null_elem) return ESP_ERR_NO_MEM;
      cJSON_AddItemToObject(*container, name.c_str(), null_elem);
    }
  }
  return container.Drop();
}

//----------------------
// Recurrent Event Timing

esp_err_t _parse_event_weekly_params(const cJSON* json, Config::Event::Weekly& container,
                                     bool strict) {
  ESP_RETURN_ON_ERROR(config::parse_and_assign_field(
      json, "daily", (Config::Event::TimeRange&)container, _timerange_parser, strict));
  ASSIGN_OR_RETURN(container.days, _weekdays_parser(cJSON_GetObjectItem(json, "weekly")));
  return ESP_OK;
}

esp_err_t _marshal_event_weekly_params(utils::AutoReleaseRes<cJSON*>& container,
                                       const Config::Event::Weekly& base,
                                       const Config::Event::Weekly& update) {
  ESP_RETURN_ON_ERROR(
      config::diff_and_marshal_field(container, "daily", (const Config::Event::TimeRange&)base,
                                     (const Config::Event::TimeRange&)update, _marshal_timerange));
  ESP_RETURN_ON_ERROR(config::diff_and_marshal_field(container, "weekly", base.days, update.days,
                                                     _marshal_weekdays));
  return ESP_OK;
}

esp_err_t _parse_event_annual_params(const cJSON* json, Config::Event::Annual& container,
                                     bool strict) {
  ESP_RETURN_ON_ERROR(config::parse_and_assign_field(
      json, "daily", (Config::Event::TimeRange&)container, _timerange_parser, strict));
  ASSIGN_OR_RETURN(container.date, _dayofyear_parser(cJSON_GetObjectItem(json, "annual")));
  return ESP_OK;
}

esp_err_t _marshal_event_annual_params(utils::AutoReleaseRes<cJSON*>& container,
                                       const Config::Event::Annual& base,
                                       const Config::Event::Annual& update) {
  ESP_RETURN_ON_ERROR(
      config::diff_and_marshal_field(container, "daily", (const Config::Event::TimeRange&)base,
                                     (const Config::Event::TimeRange&)update, _marshal_timerange));
  ESP_RETURN_ON_ERROR(config::diff_and_marshal_field(container, "annual", base.date, update.date,
                                                     _marshal_dayofyear));
  return ESP_OK;
}

//----------------------
// Event transitions

utils::DataOrError<std::vector<std::string>> _transitions_parser(cJSON* item) {
  std::vector<std::string> transitions;
  if (!cJSON_IsArray(item)) {
    ESP_LOGD(TAG, "Transitions not an array");
    return ESP_ERR_INVALID_ARG;
  }
  for (int i = 0; i < cJSON_GetArraySize(item); ++i) {
    if (!cJSON_IsString(cJSON_GetArrayItem(item, i))) {
      ESP_LOGD(TAG, "Transition not a string");
      return ESP_ERR_INVALID_ARG;
    }
    transitions.emplace_back(cJSON_GetStringValue(cJSON_GetArrayItem(item, i)));
  }
  return transitions;
}

utils::DataOrError<cJSON*> _marshal_transitions(const std::vector<std::string>& base,
                                                const std::vector<std::string>& update) {
  utils::AutoReleaseRes<cJSON*> container(cJSON_CreateArray(), cJSON_Delete);
  if (!*container) return ESP_ERR_NO_MEM;

  size_t diff_count = 0;
  for (size_t i = 0; i < std::max(base.size(), update.size()); ++i) {
    if (i >= base.size() || i >= update.size() || base[i] != update[i]) ++diff_count;
    if (i < update.size()) {
      cJSON* transition_elem = cJSON_CreateString(update[i].c_str());
      if (!transition_elem) return ESP_ERR_NO_MEM;

      cJSON_AddItemToArray(*container, transition_elem);
    }
  }
  return diff_count ? container.Drop() : nullptr;
}

//----------------------
// Event params

esp_err_t _parse_event(const cJSON* json, Config::Event& container, bool strict) {
  PARSE_AND_ASSIGN_FIELD(json, container, type,
                         (config::string_decoder<Config::Event::Type, _decode_event_type>), strict);
  PARSE_AND_ASSIGN_FIELD(json, container, transitions, _transitions_parser, strict);

  switch (container.type) {
    case Config::Event::Type::RECURRENT_DAILY: {
      PARSE_AND_ASSIGN_FIELD(json, container, daily, _timerange_parser, strict);
    } break;

    case Config::Event::Type::RECURRENT_WEEKLY: {
      ESP_RETURN_ON_ERROR(_parse_event_weekly_params(json, container.weekly, strict));
    } break;

    case Config::Event::Type::RECURRENT_ANNUAL: {
      ESP_RETURN_ON_ERROR(_parse_event_annual_params(json, container.annual, strict));
    } break;

    default:
      return ESP_ERR_NOT_SUPPORTED;
  }

  return ESP_OK;
}

std::string _print_event(const Config::Event& event) {
  if (!event) return "(invalid event)";

  std::string result;
  utils::DataBuf PrintBuf(64);
  result.append(_encode_event_type(event.type)).append(" | ");
  switch (event.type) {
    case Config::Event::Type::RECURRENT_DAILY: {
      result.append(_print_time_range(event.daily));
      break;
    }
    case Config::Event::Type::RECURRENT_WEEKLY: {
      result.append(_print_weekdays(event.weekly.days).c_str());
      result.append(", ").append(_print_time_range(event.weekly));
      break;
    }
    case Config::Event::Type::RECURRENT_ANNUAL: {
      result.append(_print_dayofyear(event.annual.date));
      result.append(", ").append(_print_time_range(event.annual));
      break;
    }
    default:
      result.append("(unexpected event)");
  }
  result.append(" | ");
  if (!event.transitions.empty()) {
    for (const auto& transition : event.transitions) result.append(transition).append(" -> ");
    result.resize(result.size() - 4);
  }
  return result;
}

esp_err_t _marshal_event(utils::AutoReleaseRes<cJSON*>& container, const Config::Event& base,
                         const Config::Event& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, type,
                         (config::string_encoder<Config::Event::Type, _encode_event_type>));
  DIFF_AND_MARSHAL_FIELD(container, base, update, transitions, _marshal_transitions);

  switch (update.type) {
    case Config::Event::Type::RECURRENT_DAILY: {
      DIFF_AND_MARSHAL_FIELD(container, base, update, daily, _marshal_timerange);
    } break;

    case Config::Event::Type::RECURRENT_WEEKLY: {
      ESP_RETURN_ON_ERROR(_marshal_event_weekly_params(container, base.weekly, update.weekly));
    } break;

    case Config::Event::Type::RECURRENT_ANNUAL: {
      ESP_RETURN_ON_ERROR(_marshal_event_annual_params(container, base.annual, update.annual));
    } break;

    default:
      return ESP_ERR_NOT_SUPPORTED;
  }

  return ESP_OK;
}

//----------------------
// Events list

esp_err_t _parse_events_list(const cJSON* json, Config::EventsList& events, bool strict) {
  if (!cJSON_IsArray(json)) {
    ESP_LOGD(TAG, "Events not an array");
    if (strict) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
  }

  size_t event_idx = 0;
  for (; event_idx < cJSON_GetArraySize(json); ++event_idx) {
    cJSON* item = cJSON_GetArrayItem(json, event_idx);
    if (!cJSON_IsObject(item)) {
      ESP_LOGD(TAG, "Event not an object");
      if (strict) return ESP_ERR_INVALID_ARG;
      continue;
    }

    Config::Event* event;
    if (event_idx < events.size()) {
      event = &events[event_idx];
    } else {
      events.emplace_back(Config::Event{});
      event = &events.back();
    }
    ESP_RETURN_ON_ERROR(_parse_event(item, *event, strict));
  }
  if (event_idx < events.size()) events.resize(event_idx);
  return ESP_OK;
}

utils::DataOrError<cJSON*> _marshal_events_list(const Config::EventsList& base,
                                                const Config::EventsList& update) {
  utils::AutoReleaseRes<cJSON*> container(cJSON_CreateArray(), cJSON_Delete);
  if (!*container) return ESP_ERR_NO_MEM;

  size_t diff_count = 0;
  for (size_t idx = 0; idx < update.size(); ++idx) {
    if (idx >= base.size() || base[idx] != update[idx]) ++diff_count;
    utils::AutoReleaseRes<cJSON*> event(cJSON_CreateObject(), cJSON_Delete);
    if (!*event) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(
        _marshal_event(event, (idx < base.size() ? base[idx] : Config::Event{}), update[idx]));
    cJSON_AddItemToArray(*container, event.Drop());
  }
  return diff_count ? container.Drop() : nullptr;
}

}  // namespace

esp_err_t parse_config(const cJSON* json, Config& container, bool strict) {
  PARSE_AND_ASSIGN_FIELD(json, container, num_pixels,
                         (config::string_decoder<size_t, config::decode_size>), strict);
  PARSE_FIELD_INPLACE(json, container, transitions, _parse_transitions_map, strict);
  PARSE_FIELD_INPLACE(json, container, events, _parse_events_list, strict);

  return ESP_OK;
}

void log_config(const Config& config) {
  if (config) {
    ESP_LOGI(TAG, "- TWiLight config:");
    ESP_LOGI(TAG, "  Number of pixels: %d", config.num_pixels);
    if (!config.transitions.empty()) {
      ESP_LOGI(TAG, "  %d Transitions:", config.transitions.size());
      for (const auto& [name, transition] : config.transitions) {
        ESP_LOGI(TAG, "    '%s': %s", name.c_str(), _print_transition(transition).c_str());
      }
    } else {
      ESP_LOGI(TAG, "  No transitions defined");
    }
    if (!config.events.empty()) {
      ESP_LOGI(TAG, "  %d Events:", config.events.size());
      for (size_t i = 0; i < config.events.size(); ++i) {
        ESP_LOGI(TAG, "    %02d. %s", i, _print_event(config.events[i]).c_str());
      }
    } else {
      ESP_LOGI(TAG, "  No events defined");
    }
  } else {
    ESP_LOGI(TAG, "- TWiLight unconfigured!");
  }
}

esp_err_t marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                         const Config& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, num_pixels,
                         (config::string_encoder<size_t, config::encode_size>));
  DIFF_AND_MARSHAL_FIELD(container, base, update, transitions, _marshal_transitions_map);
  DIFF_AND_MARSHAL_FIELD(container, base, update, events, _marshal_events_list);

  return ESP_OK;
}

std::string print_transition(const Config::Transition& transition) {
  return _print_transition(transition);
}

std::string print_time(uint16_t time) { return _print_time(time); }
std::string print_event(const Config::Event& event) { return _print_event(event); }

utils::DataOrError<Config::Transition> parse_transition(const cJSON* json, bool strict) {
  Config::Transition transition;
  ESP_RETURN_ON_ERROR(_parse_transition(json, transition, strict));
  return transition;
}

utils::DataOrError<Config::TransitionsMap> parse_transitions_map(const cJSON* json, bool strict) {
  Config::TransitionsMap transitions;
  ESP_RETURN_ON_ERROR(_parse_transitions_map(json, transitions, strict));
  return transitions;
}

utils::DataOrError<Config::EventsList> parse_events_list(const cJSON* json, bool strict) {
  Config::EventsList events;
  ESP_RETURN_ON_ERROR(_parse_events_list(json, events, strict));
  return events;
}

}  // namespace zw::esp8266::app::twilight
