
#ifndef APP_TWILIGHT_INTERFACE
#define APP_TWILIGHT_INTERFACE

#include <map>

#include "esp_err.h"

#include "ZWUtils.hpp"

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

extern esp_err_t parse_config(const cJSON* json, Config& container, bool strict = false);
extern esp_err_t parse_transition(const cJSON* json, Config::Transition& container,
                                  bool strict = false);

extern void log_config(const Config& config);
extern void log_transition(const Config::Transition& transition);

extern esp_err_t marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                                const Config& update);
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

#endif  // APP_TWILIGHT_INTERFACE
