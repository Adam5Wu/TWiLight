
#ifndef APP_TWILIGHT_INTERFACE
#define APP_TWILIGHT_INTERFACE

#include <stddef.h>
#include <string>
#include <map>
#include <vector>

#include "LSPixel.hpp"

namespace zw::esp8266::app::twilight {

struct Config {
  size_t num_pixels;

  struct Transition {
    enum class Type {
      UNSPECIFIED = 0,
      UNIFORM_COLOR,
      COLOR_WIPE,
      COLOR_WHEEL,
    } type;

    uint32_t duration_ms;

    struct UniformColor {
      lightshow::RGB888 color;
    };

    struct ColorWipe {
      enum class Direction {
        UNSPECIFIED = 0,
        LeftToRight,
        RightToLeft,
      } direction;
      float blade_width;
      lightshow::RGB888 color;
    };

    struct ColorWheel {
      uint16_t start_hue;
      float wheel_width;
      uint16_t intensity;
    };

    union {
      UniformColor uniform_color;
      ColorWipe color_wipe;
      ColorWheel color_wheel;
    };

    operator bool() const { return duration_ms > 0; }
  };
  using TransitionsMap = std::map<std::string, Transition>;
  TransitionsMap transitions;

  struct Event {
    enum class Type {
      UNSPECIFIED = 0,
      RECURRENT_DAILY,
      RECURRENT_WEEKLY,
      RECURRENT_ANNUAL,
    } type;

    struct TimeRange {
      uint16_t start;
      uint16_t end;
      bool has_end;
    };

    struct Weekly : TimeRange {
      enum class Day : uint8_t {
        SUNDAY = 0x01,
        MONDAY = 0x02,
        TUESDAY = 0x04,
        WEDNESDAY = 0x08,
        THURSDAY = 0x10,
        FRIDAY = 0x20,
        SATURDAY = 0x40
      };

      uint8_t days;
    };

    struct DayOfYear {
      uint8_t month_idx;
      uint8_t day;
    };

    struct Annual : TimeRange {
      DayOfYear date;
    };

    union {
      TimeRange daily;
      Weekly weekly;
      Annual annual;
    };

    std::vector<std::string> transitions;

    operator bool() const { return !transitions.empty(); }
  };
  using EventsList = std::vector<Event>;
  EventsList events;

  operator bool() const { return num_pixels > 0; }
};

}  // namespace zw::esp8266::app::twilight

#endif  // APP_TWILIGHT_INTERFACE
