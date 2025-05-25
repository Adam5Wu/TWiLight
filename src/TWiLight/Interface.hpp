
#ifndef APP_TWILIGHT_INTERFACE
#define APP_TWILIGHT_INTERFACE

#include <stddef.h>
#include <string>
#include <map>

#include "LSPixel.hpp"

namespace zw::esp8266::app::twilight {

struct Config {
  size_t num_pixels;

  struct Transition {
    enum class Type {
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
        LeftToRight,
        RightToLeft,
      } direction;
      float blade_width;
      lightshow::RGB888 color;
    };

    struct ColorWheel {
      uint16_t start_hue;
      uint16_t end_hue;
      uint16_t intensity;
    };

    union {
      UniformColor uniform_color;
      ColorWipe color_wipe;
      ColorWheel color_wheel;
    };

    operator bool() const { return duration_ms > 0; }
  };
  std::map<std::string, Transition> transitions;

  operator bool() const { return num_pixels > 0; }
};

}  // namespace zw::esp8266::app::twilight

#endif  // APP_TWILIGHT_INTERFACE
