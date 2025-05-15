#include "Module.hpp"

#include <string>

#include "esp_log.h"
#include "esp_err.h"

#include "FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "cJSON.h"

#include "ZWUtils.hpp"

#include "LSRenderer.hpp"
#include "LSDriver.hpp"

#include "ZWAppUtils.hpp"

#include "AppConfig/Interface.hpp"
#include "AppEventMgr/Interface.hpp"
#include "AppHTTPD/Interface.hpp"

#include "Interface.hpp"
#include "Interface_Private.hpp"
#include "HTTPD_Handler_TWiLight.hpp"

#define HTTPD_STARTUP_TIMEOUT (3 * CONFIG_FREERTOS_HZ)  // 3 sec

#define TWILIGHT_DEFAULT_PIXELS 16

#define TWILIGHT_LIGHTSHOW_JITTER_BUFFER_US 1800
#define TWILIGHT_LIGHTSHOW_TASK_STACK_SIZE LS::kDefaultTaskStack
#define TWILIGHT_LIGHTSHOW_TASK_PRIORITY LS::kDefaultTaskPriority

namespace zw::esp8266::app::twilight {
namespace {

inline constexpr char TAG[] = "TWiLight";

using config::AppConfig;

namespace LS = ::zw::esp8266::lightshow;

inline constexpr LS::RGB888 TWILIGHT_PIXEL_SETUP_HIGH_COLOR = {0x16, 0x32, 0x08};
inline constexpr LS::RGB888 TWILIGHT_PIXEL_SETUP_LOW_COLOR = {0x02, 0x04, 0x01};
inline constexpr float TWILIGHT_PIXEL_SETUP_BLADE_WIDTH = 0.2;
inline constexpr uint32_t TWILIGHT_PIXEL_SETUP_BLADE_WIPE_MS_BASE = 500;
inline constexpr uint32_t TWILIGHT_PIXEL_SETUP_BLADE_WIPE_MS_PER_PIXEL = 3;
inline constexpr uint32_t TWILIGHT_PIXEL_SETUP_BLADE_FADE_MS = 1200;

inline constexpr LS::RGB888 TWILIGHT_TRANSITION_SETUP_ON_COLOR = {0x32, 0x32, 0x32};
inline constexpr uint32_t TWILIGHT_TRANSITION_COOLDOWN_MS = 500;

Config config_;

inline constexpr EventBits_t TWILIGHT_STATUS_RUNNING = BIT0;

inline constexpr EventBits_t TWILIGHT_STATUS_SETUP_PIXELS = BIT16;
inline constexpr EventBits_t TWILIGHT_STATUS_SETUP_TRANSITION = BIT17;
inline constexpr EventBits_t TWILIGHT_STATUS_SETUP_MASK = BIT16 | BIT17;

struct {
  EventGroupHandle_t status;
  SemaphoreHandle_t strip_lock;
  SemaphoreHandle_t state_lock;

  LS::IOConfig io_config;
  std::unique_ptr<LS::Renderer> renderer;
  TaskHandle_t service_task_handle_;

  std::optional<Config> config_setup;
  Config::Transition test_transition;
} state_ = {};

esp_err_t _start_lightshow_driver(const LS::IOConfig& io_config, LS::Renderer* renderer) {
  // Assume holding strip_lock
  ESP_RETURN_ON_ERROR(
      renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
  ESP_RETURN_ON_ERROR(LS::DriverSetup(io_config, renderer));
  ESP_RETURN_ON_ERROR(LS::DriverStart(/*task_stack=*/TWILIGHT_LIGHTSHOW_TASK_STACK_SIZE,
                                      /*task_priority=*/TWILIGHT_LIGHTSHOW_TASK_PRIORITY));
  return ESP_OK;
}

esp_err_t _effect_pixel_setup(LS::Renderer* renderer, size_t num_pixels) {
  ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(LS::WiperTarget::Create(
      TWILIGHT_PIXEL_SETUP_BLADE_WIPE_MS_BASE +
          TWILIGHT_PIXEL_SETUP_BLADE_WIPE_MS_PER_PIXEL * num_pixels,
      LS::WiperTarget::ColorWipeConfig(TWILIGHT_PIXEL_SETUP_BLADE_WIDTH,
                                       TWILIGHT_PIXEL_SETUP_HIGH_COLOR,
                                       LS::WiperTarget::Direction::LeftToRight))));
  ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(LS::UniformColorTarget::Create(
      TWILIGHT_PIXEL_SETUP_BLADE_FADE_MS, TWILIGHT_PIXEL_SETUP_LOW_COLOR)));
  return ESP_OK;
}

esp_err_t _effect_transition_setup(LS::Renderer* renderer, const Config::Transition& transition) {
  switch (transition.type) {
    case Config::Transition::Type::UNIFORM_COLOR: {
      const auto& params = transition.uniform_color;
      if (params.color != LS::RGB8BPixel::BLACK()) {
        ESP_RETURN_ON_ERROR(
            renderer->EnqueueOrError(LS::UniformColorTarget::Create(0, LS::RGB8BPixel::BLACK())));
        ESP_RETURN_ON_ERROR(
            renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
        ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(
            LS::UniformColorTarget::Create(transition.duration_ms, params.color)));
        ESP_RETURN_ON_ERROR(
            renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
      }
      ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(
          LS::UniformColorTarget::Create(0, TWILIGHT_TRANSITION_SETUP_ON_COLOR)));
      ESP_RETURN_ON_ERROR(
          renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
      ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(
          LS::UniformColorTarget::Create(transition.duration_ms, params.color)));
      ESP_RETURN_ON_ERROR(
          renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
    } break;

    case Config::Transition::Type::COLOR_WIPE: {
      ESP_LOGW(TAG, "Unimplemented");
    } break;

    case Config::Transition::Type::COLOR_WHEEL: {
      ESP_LOGW(TAG, "Unimplemented");
    } break;

    default:
      ESP_LOGW(TAG, "Unrecognized transition type");
  }

  return ESP_OK;
}

void _twilight_task(TimerHandle_t) {
  while (true) {
    {
      std::optional<Config> config_setup;
      {
        ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
        config_setup = state_.config_setup;
      }

      {
        ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.strip_lock);
        if (eventmgr::IsSystemFailed()) break;
        bool lightshow_action = false;

        if (config_setup.has_value()) {
          // We are in set up mode
          EventBits_t cur_status = xEventGroupGetBits(state_.status);

          if (cur_status & TWILIGHT_STATUS_SETUP_PIXELS) {
            ESP_GOTO_ON_ERROR(
                _effect_pixel_setup(state_.renderer.get(), state_.config_setup->num_pixels),
                failure);
            lightshow_action = true;
          } else if (cur_status & TWILIGHT_STATUS_SETUP_TRANSITION) {
            ESP_GOTO_ON_ERROR(
                _effect_transition_setup(state_.renderer.get(), state_.test_transition), failure);
            lightshow_action = true;
          }
        }

        if (lightshow_action) {
          // Wait for transition to finish before releasing strip lock
          state_.renderer->WaitFor(LS::RENDERER_IDLE_TARGET, portMAX_DELAY);
          LS::IOStats io_stats = LS::DriverStats();
          ESP_LOGD(TAG, "Observed %d underflows, %d near-misses", io_stats.underflow_actual,
                   io_stats.underflow_near_miss);
#if ISR_DEVELOPMENT
          ESP_LOGI(TAG, "ISR latency [%d, %d], late wakeup %d times",
                   io_stats.isr_process_latency_low / g_esp_ticks_per_us,
                   io_stats.isr_process_latency_high / g_esp_ticks_per_us,
                   io_stats.isr_late_wakeup);
#endif
        } else {
          // Nothing to do, just sleep for a tick.
          vTaskDelay(1);
        }
      }
    }
  }
  return;

failure:
  eventmgr::SetSystemFailed();
}

void _twilight_task_event(int32_t event_id, void*, void*) {
  switch (event_id) {
    case ZW_SYSTEM_EVENT_NET_STA_IP_READY:
      xEventGroupSetBits(state_.status, TWILIGHT_STATUS_RUNNING);

      ESP_LOGD(TAG, "Starting LightShow driver...");
      ESP_GOTO_ON_ERROR(_start_lightshow_driver(state_.io_config, state_.renderer.get()), failure);

      ESP_LOGD(TAG, "Starting service task...");
      ESP_GOTO_ON_ERROR(xTaskCreate(ZWTaskWrapper<TAG, _twilight_task>, "twilight_serivce", 3000,
                                    NULL, 5, &state_.service_task_handle_) == pdPASS
                            ? ESP_OK
                            : ESP_FAIL,
                        failure);

      if (!config_) {
        ESP_LOGI(TAG, "Strip size not configured, entering setup mode...");
        Setup_Enter();
      }
      break;

    default:
      ESP_LOGW(TAG, "Unrecognized event %d", event_id);
  }
  return;

failure:
  eventmgr::SetSystemFailed();
}

esp_err_t _reconfigure_lightshow(const Config& config) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.strip_lock);

  // Turn off all currently configured pixels.
  ESP_RETURN_ON_ERROR(
      state_.renderer->EnqueueOrError(LS::UniformColorTarget::Create(0, LS::RGB8BPixel::BLACK())));
  state_.renderer->WaitFor(LS::RENDERER_IDLE_TARGET, portMAX_DELAY);

  ESP_RETURN_ON_ERROR(LS::DriverStop());
  ASSIGN_OR_RETURN(auto new_renderer, LS::Renderer::Create(config.num_pixels));
  ESP_RETURN_ON_ERROR(_start_lightshow_driver(state_.io_config, new_renderer.get()));
  state_.renderer = std::move(new_renderer);

  return ESP_OK;
}

esp_err_t _init_twilight(void) {
  state_.status = xEventGroupCreate();
  if (state_.status == NULL) {
    ESP_LOGE(TAG, "Failed to create status event group!");
    return ESP_ERR_NO_MEM;
  }

  state_.strip_lock = xSemaphoreCreateMutex();
  if (state_.strip_lock == NULL) {
    ESP_LOGE(TAG, "Failed to create strip access lock!");
    return ESP_ERR_NO_MEM;
  }

  state_.state_lock = xSemaphoreCreateMutex();
  if (state_.state_lock == NULL) {
    ESP_LOGE(TAG, "Failed to create state access lock!");
    return ESP_ERR_NO_MEM;
  }

  config_ = config::get()->twilight;

  ESP_LOGD(TAG, "Setting up LightShow...");
  ASSIGN_OR_RETURN(state_.renderer,
                   LS::Renderer::Create(config_ ? config_.num_pixels : TWILIGHT_DEFAULT_PIXELS));

  // TODO: Expose IOConfig fields as config entries.
  state_.io_config = LS::CONFIG_WS2812_NEW(TWILIGHT_LIGHTSHOW_JITTER_BUFFER_US);

  // Add HTTPD handler registrar
  httpd::add_ext_handler_registrar(register_httpd_handler);

  // Wait for Appliance enter regular serving
  return eventmgr::system_event_register_handler(
      ZW_SYSTEM_EVENT_NET_STA_IP_READY,
      eventmgr::SystemEventHandlerWrapper<TAG, _twilight_task_event>);
}

Config& _get_config(AppConfig& config) { return config.twilight; }

#define PARSE_AND_ASSIGN_FIELD(json, container, field, parse_func, strict) \
  ESP_RETURN_ON_ERROR(                                                     \
      config::parse_and_assign_field(json, #field, container.field, parse_func, strict))

esp_err_t _parse_config(const cJSON* json, Config& container, bool strict) {
  PARSE_AND_ASSIGN_FIELD(json, container, num_pixels,
                         (config::string_decoder<size_t, config::decode_size>), strict);

  return ESP_OK;
}

void _log_config(const Config& config) {
  if (config) {
    ESP_LOGI(TAG, "- TWiLight config:");
    ESP_LOGI(TAG, "  Number of pixels: %d", config.num_pixels);
  } else {
    ESP_LOGI(TAG, "- TWiLight unconfigured!");
  }
}

#define DIFF_AND_MARSHAL_FIELD(container, base, update, field, marshal_func) \
  ESP_RETURN_ON_ERROR(                                                       \
      config::diff_and_marshal_field(container, #field, base.field, update.field, marshal_func))

esp_err_t _marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                          const Config& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, num_pixels,
                         (config::string_encoder<size_t, config::encode_size>));

  return ESP_OK;
}

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

utils::DataOrError<LS::RGB888> _decode_RGB8BHex(const char* str) {
  if (*str++ != '#') return ESP_ERR_INVALID_ARG;

  LS::RGB888 color;
  ASSIGN_OR_RETURN(color.r, utils::ParseHexByte(str));
  ASSIGN_OR_RETURN(color.g, utils::ParseHexByte(str + 2));
  ASSIGN_OR_RETURN(color.b, utils::ParseHexByte(str + 4));
  return color;
}

esp_err_t _parse_transition_uniform_color(const cJSON* json,
                                          Config::Transition::UniformColor& container,
                                          bool strict) {
  PARSE_AND_ASSIGN_FIELD(json, container, color,
                         (config::string_decoder<LS::RGB888, _decode_RGB8BHex>), strict);
  return ESP_OK;
}

std::string _encode_RGB8BHex(const LS::RGB888& color) {
  return utils::DataBuf(8).PrintTo("#%02x%02x%02x", color.r, color.g, color.b);
}

esp_err_t _marshal_transition_uniform_color(utils::AutoReleaseRes<cJSON*>& container,
                                            const Config::Transition::UniformColor& base,
                                            const Config::Transition::UniformColor& update) {
  DIFF_AND_MARSHAL_FIELD(container, base, update, color,
                         (config::string_encoder<LS::RGB888, _encode_RGB8BHex>));

  return ESP_OK;
}

}  // namespace

esp_err_t marshal_config(utils::AutoReleaseRes<cJSON*>& container, const Config& base,
                         const Config& update) {
  return _marshal_config(container, base, update);
}

esp_err_t parse_transition(const cJSON* json, Config::Transition& container, bool strict) {
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

void log_transition(const Config::Transition& transition) {
  if (transition) {
    std::string transition_name = _encode_transition_type(transition.type);
    ESP_LOGI(TAG, "- %s transition:", transition_name.c_str());
    ESP_LOGI(TAG, "  Duration: %dms", transition.duration_ms);
    switch (transition.type) {
      case Config::Transition::Type::UNIFORM_COLOR: {
        ESP_LOGI(TAG, "  Color: %s", LS::to_string(transition.uniform_color.color).c_str());
        break;
      }
      case Config::Transition::Type::COLOR_WIPE: {
        ESP_LOGI(TAG, "  Color: %s", LS::to_string(transition.color_wipe.color).c_str());
        ESP_LOGI(TAG, "  Direction: %s", "...");
        ESP_LOGI(TAG, "  Blade width: %d%%", 100);
        break;
      }
      case Config::Transition::Type::COLOR_WHEEL: {
        ESP_LOGI(TAG, "  Start Hue: %ddeg", 100);
        ESP_LOGI(TAG, "  End Hue: %ddeg", 100);
        ESP_LOGI(TAG, "  Intensity: %d%%", 100);
        break;
      }
    }
  }
}

esp_err_t marshal_transition(utils::AutoReleaseRes<cJSON*>& container,
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

utils::DataOrError<ConfigState> GetConfigState(void) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);

  ConfigState config_state;
  if ((config_state.setup = state_.config_setup.has_value())) {
    config_state.config = *state_.config_setup;
  } else {
    config_state.config = config_;
  }
  return config_state;
}

utils::ESPErrorStatus Setup_Enter(void) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if ((xEventGroupGetBits(state_.status) & TWILIGHT_STATUS_RUNNING) == 0) {
    return {"Not in serving mode"};
  }

  if (state_.config_setup.has_value()) {
    return {ESP_OK, "Already in setup mode"};
  }

  state_.config_setup = config_;
  if (!config_) {
    state_.config_setup->num_pixels = TWILIGHT_DEFAULT_PIXELS;
  }

  // Provide visual indication
  xEventGroupSetBits(state_.status, TWILIGHT_STATUS_SETUP_PIXELS);
  return ESP_OK;
}

utils::ESPErrorStatus Setup_Exit(bool save_changes) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (!state_.config_setup.has_value()) {
    return {"Not in setup mode"};
  }

  // Cannot exit setup until strip size is finalized and saved
  if (!*state_.config_setup) {
    return {"Strip size is not set"};
  }
  if (!config_ && !save_changes) {
    return {"Strip size is not saved"};
  }

  if (save_changes) {
    config_ = *state_.config_setup;
    {
      auto new_config = config::get();
      new_config->twilight = config_;
      ESP_RETURN_ON_ERROR(config::persist());
    }
  } else {
    // Restore current strip setup
    if (state_.config_setup->num_pixels != config_.num_pixels) {
      auto result = _reconfigure_lightshow(config_);
      if (result != ESP_OK) {
        ESP_LOGE(TAG, "LightShow reconfiguration failed!");
        eventmgr::SetSystemFailed();
      }
    }
  }

  xEventGroupClearBits(state_.status, TWILIGHT_STATUS_SETUP_MASK);
  state_.config_setup.reset();

  //...
  return ESP_OK;
}

utils::ESPErrorStatus Setup_StripSize(size_t num_pixels) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (!state_.config_setup.has_value()) {
    return {"Not in setup mode"};
  }

  if (num_pixels < 1 || num_pixels > LS::kMaxStripSize) {
    return {"Invalid strip size"};
  }

  if (num_pixels == state_.config_setup->num_pixels) {
    return {ESP_OK, "Strip size unchanged"};
  }

  xEventGroupClearBits(state_.status, TWILIGHT_STATUS_SETUP_MASK);
  xEventGroupSetBits(state_.status, TWILIGHT_STATUS_SETUP_PIXELS);
  state_.config_setup->num_pixels = num_pixels;

  auto result = _reconfigure_lightshow(*state_.config_setup);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "LightShow reconfiguration failed!");
    eventmgr::SetSystemFailed();
  }

  return ESP_OK;
}

utils::ESPErrorStatus Setup_TestTransition(const Config::Transition& transition) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (!state_.config_setup.has_value()) {
    return {"Not in setup mode"};
  }

  xEventGroupClearBits(state_.status, TWILIGHT_STATUS_SETUP_MASK);
  xEventGroupSetBits(state_.status, TWILIGHT_STATUS_SETUP_TRANSITION);
  state_.test_transition = transition;

  return ESP_OK;
}

esp_err_t config_init(void) {
  ESP_LOGD(TAG, "Initializing for config...");
  return config::register_custom_field("twilight", _get_config, _parse_config, _log_config,
                                       _marshal_config);
}

esp_err_t init(void) {
  ESP_LOGD(TAG, "Initializing...");
  ESP_RETURN_ON_ERROR(_init_twilight());
  return ESP_OK;
}

void finit(void) {
  // Stop the light-show driver and service task
  LS::DriverStop();
  if (state_.service_task_handle_ != NULL) {
    vTaskDelete(state_.service_task_handle_);
  }
}

}  // namespace zw::esp8266::app::twilight
