#include "Module.hpp"

#include <optional>
#include <vector>
#include <deque>
#include <string>
#include <utility>

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
#include "AppTime/Interface.hpp"
#include "AppHTTPD/Interface.hpp"

#include "Interface.hpp"
#include "Interface_Private.hpp"

#include "Config.hpp"
#include "HTTPD_Handler.hpp"
#include "EventSequencer.hpp"

#define HTTPD_STARTUP_TIMEOUT (3 * CONFIG_FREERTOS_HZ)  // 3 sec

#define TWILIGHT_DEFAULT_PIXELS 16
#define TWILIGHT_TARGET_FPS 60
#define TWILIGHT_RENDERER_BLEND_MODE LS::Renderer::BlendMode::SMOOTH_4X4R
#define TWILIGHT_TASK_IDLE (CONFIG_FREERTOS_HZ / 10)

#define TWILIGHT_LIGHTSHOW_JITTER_BUFFER_US 1800
#define TWILIGHT_LIGHTSHOW_TASK_STACK_SIZE LS::kDefaultTaskStack
#define TWILIGHT_LIGHTSHOW_TASK_PRIORITY LS::kDefaultTaskPriority

namespace zw::esp8266::app::twilight {
namespace {

inline constexpr char TAG[] = "TWiLight";

using config::AppConfig;

namespace LS = ::zw::esp8266::lightshow;

inline constexpr uint8_t TWILIGHT_SETUP_TEST_COUNTS = 2;

inline constexpr LS::RGB888 TWILIGHT_NO_CONFIG_COLOR = {0x14, 0x0F, 0x0A};

inline constexpr LS::RGB888 TWILIGHT_PIXEL_SETUP_HIGH_COLOR = {0x16, 0x32, 0x08};
inline constexpr LS::RGB888 TWILIGHT_PIXEL_SETUP_LOW_COLOR = {0x02, 0x04, 0x01};
inline constexpr float TWILIGHT_PIXEL_SETUP_BLADE_WIDTH = 0.2;
inline constexpr uint32_t TWILIGHT_PIXEL_SETUP_BLADE_WIPE_MS_BASE = 500;
inline constexpr uint32_t TWILIGHT_PIXEL_SETUP_BLADE_WIPE_MS_PER_PIXEL = 3;
inline constexpr uint32_t TWILIGHT_PIXEL_SETUP_BLADE_FADE_MS = 3000;

inline constexpr LS::RGB888 TWILIGHT_TRANSITION_SETUP_ON_COLOR = {0x80, 0x80, 0x80};
inline constexpr uint32_t TWILIGHT_TRANSITION_COOLDOWN_MS = 500;

Config config_;

inline constexpr EventBits_t TWILIGHT_STATUS_RUNNING = BIT0;
inline constexpr EventBits_t TWILIGHT_STATUS_INTERRUPT = BIT1;

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
  uint8_t test_countdown;

  Config::Transition manual_transition;
  // Manual override record: {start_time, duration}
  std::optional<std::pair<int32_t, int32_t>> manual_override;

  std::vector<const Config::Transition*> transitions;
  std::deque<EventEntry> event_sequence;
} state_ = {};

const Config::Transition TWILIGHT_NO_CONFIG_TRANSITION = {
    .type = Config::Transition::Type::UNIFORM_COLOR,
    .duration_ms = 1000,
    .uniform_color = {.color = TWILIGHT_NO_CONFIG_COLOR},
};

esp_err_t _start_lightshow_driver(const LS::IOConfig& io_config, LS::Renderer* renderer) {
  // Assume holding strip_lock
  ESP_RETURN_ON_ERROR(
      renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
  ESP_RETURN_ON_ERROR(LS::DriverSetup(io_config, renderer));
  ESP_RETURN_ON_ERROR(LS::DriverStart(/*task_stack=*/TWILIGHT_LIGHTSHOW_TASK_STACK_SIZE,
                                      /*task_priority=*/TWILIGHT_LIGHTSHOW_TASK_PRIORITY));
  return ESP_OK;
}

esp_err_t _setup_effect_pixel_num(LS::Renderer* renderer, size_t num_pixels) {
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

esp_err_t _setup_effect_transition(LS::Renderer* renderer, const Config::Transition& transition) {
  switch (transition.type) {
    case Config::Transition::Type::UNIFORM_COLOR: {
      const auto& params = transition.uniform_color;
      if (params.color != LS::RGB8BPixel::BLACK()) {
        ESP_RETURN_ON_ERROR(
            renderer->EnqueueOrError(LS::UniformColorTarget::Create(100, LS::RGB8BPixel::BLACK())));
        ESP_RETURN_ON_ERROR(
            renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
        ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(
            LS::UniformColorTarget::Create(transition.duration_ms, params.color)));
        ESP_RETURN_ON_ERROR(
            renderer->EnqueueOrError(LS::NoopTarget::Create(TWILIGHT_TRANSITION_COOLDOWN_MS)));
      }
      ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(
          LS::UniformColorTarget::Create(100, TWILIGHT_TRANSITION_SETUP_ON_COLOR)));
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

esp_err_t _render_transition(LS::Renderer* renderer, const Config::Transition& transition) {
  switch (transition.type) {
    case Config::Transition::Type::UNIFORM_COLOR: {
      ESP_RETURN_ON_ERROR(renderer->EnqueueOrError(
          LS::UniformColorTarget::Create(transition.duration_ms, transition.uniform_color.color)));
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

esp_err_t _render_transitions(LS::Renderer* renderer,
                              const std::vector<const Config::Transition*>& transitions) {
  for (const Config::Transition* transition : transitions) {
    ESP_RETURN_ON_ERROR(_render_transition(renderer, *transition));
  }
  return ESP_OK;
}

utils::DataOrError<std::deque<EventEntry>> _recompute_event_sequence(const struct tm& time_tm) {
  int32_t second_of_day = get_second_of_day(time_tm);

  std::vector<RawEventEntry> raw_events = generate_raw_events(config_.events, time_tm);
  if (state_.manual_override.has_value()) {
    const auto& [start_time, duration] = *state_.manual_override;
    int32_t end_time = (duration > 0) ? (start_time + duration) : -1;
    if (end_time > SECONDS_IN_A_DAY) {
      // Split the duration across mid-night
      state_.manual_override = std::make_pair(0, end_time - SECONDS_IN_A_DAY);
      end_time = SECONDS_IN_A_DAY;
    } else {
      state_.manual_override.reset();
    }
    raw_events.push_back({
        .start = start_time,
        .end = end_time,
        .event_idx = EVENT_IDX_MANUAL_OVERRIDE,
    });
  }
  return convert_raw_events(std::move(raw_events), second_of_day);
}

void _insert_config_transitions(const Config::Event& event) {
  for (const std::string& transition_name : event.transitions) {
    auto iter = config_.transitions.find(transition_name);
    if (iter != config_.transitions.end()) {
      state_.transitions.push_back(&iter->second);
    } else {
      ESP_LOGW(TAG, "Unknown transition '%s'", transition_name.c_str());
      state_.transitions.push_back(&TWILIGHT_NO_CONFIG_TRANSITION);
    }
  }
}

void _insert_non_config_transitions(int16_t event_idx) {
  switch (event_idx) {
    case EVENT_IDX_UNCONFIGURED:
      state_.transitions.push_back(&TWILIGHT_NO_CONFIG_TRANSITION);
      break;

    case EVENT_IDX_MANUAL_OVERRIDE:
      state_.transitions.push_back(&state_.manual_transition);
      break;

    default:
      ESP_LOGW(TAG, "Unrecognized event index %d", event_idx);
      break;
  }
}

esp_err_t _check_events() {
  if (eventmgr::system_states_peek(ZW_SYSTEM_STATE_TIME_NTP_TRACKING |
                                   ZW_SYSTEM_STATE_TIME_ALIGNED) !=
      (ZW_SYSTEM_STATE_TIME_NTP_TRACKING | ZW_SYSTEM_STATE_TIME_ALIGNED)) {
    // Do not act based on time if it is unanchored.
    return ESP_OK;
  }
  ASSIGN_OR_RETURN(struct tm time_tm, time::GetLocalTime());
  int32_t second_of_day = get_second_of_day(time_tm);

  bool new_transitions = state_.event_sequence.empty();
  int16_t last_event_idx = EVENT_IDX_UNINITIALIZED;
  // Regular serving will enter this loop when the current event has completed.
  // But if we come from exiting setup, the event_sequence is already cleared.
  while (!state_.event_sequence.empty()) {
    const EventEntry& entry = state_.event_sequence.front();
    if (entry.completion < second_of_day ||
        (second_of_day == 0 && entry.completion == SECONDS_IN_A_DAY)) {
      if (last_event_idx == EVENT_IDX_UNINITIALIZED) last_event_idx = entry.event_idx;
      state_.event_sequence.pop_front();
      new_transitions = true;
      continue;
    }
    break;
  }
  if (state_.event_sequence.empty()) {
    ASSIGN_OR_RETURN(state_.event_sequence, _recompute_event_sequence(time_tm));
  }

  if (new_transitions) {
    const int16_t event_idx = state_.event_sequence.front().event_idx;
    // In regular serving, it is possible that a scheduled event is split into
    // multiple windows. If we are roaming across windows of the same event,
    // there is no need to re-run its transitions.
    if (event_idx != last_event_idx) {
      if (event_idx < 0) {
        // For non-configured event, the index denotes the type
        _insert_non_config_transitions(event_idx);
      } else {
        // Run event transitions according to its configuration.
        _insert_config_transitions(config_.events[event_idx]);
      }
    }
  }
  return ESP_OK;
}

void _twilight_task(TimerHandle_t) {
  while (true) {
    std::optional<Config> config_setup;
    {
      ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
      config_setup = state_.config_setup;
      xEventGroupClearBits(state_.status, TWILIGHT_STATUS_INTERRUPT);
    }

    {
      ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.strip_lock);
      if (eventmgr::IsSystemFailed()) break;
      bool lightshow_action = false;

      LS::Renderer* renderer = state_.renderer.get();
      EventBits_t cur_status = xEventGroupGetBits(state_.status);
      if (config_setup.has_value()) {
        // We are in set up mode
        if (cur_status & TWILIGHT_STATUS_SETUP_PIXELS) {
          ESP_GOTO_ON_ERROR(_setup_effect_pixel_num(renderer, state_.config_setup->num_pixels),
                            failure);
          lightshow_action = true;
        } else if (cur_status & TWILIGHT_STATUS_SETUP_TRANSITION) {
          ESP_GOTO_ON_ERROR(_setup_effect_transition(renderer, state_.test_transition), failure);
          lightshow_action = true;
          if (--state_.test_countdown == 0)
            xEventGroupClearBits(state_.status, TWILIGHT_STATUS_SETUP_TRANSITION);
        }
        // Since setup will likely change the active events and/or presentation of a transition
        // We'd better invalidate previously computed event sequence -- after exiting the setup,
        // the event sequence will be re-computed and the transitions will also be re-rendered.
        state_.event_sequence.clear();
        // Setup will terminate any on-going manual override.
        state_.manual_override.reset();
      } else {
        // In regular service mode
        ESP_GOTO_ON_ERROR(_check_events(), failure);
        if (!state_.transitions.empty()) {
          ESP_GOTO_ON_ERROR(_render_transitions(renderer, state_.transitions), failure);
          state_.transitions.clear();
          lightshow_action = true;
        }
      }

      if (lightshow_action) {
        // Wait for transition to finish before releasing strip lock
        while (state_.renderer->WaitFor(LS::RENDERER_IDLE_TARGET, 1) == 0) {
          if (xEventGroupWaitBits(state_.status, TWILIGHT_STATUS_INTERRUPT, true, false, 0) &
              TWILIGHT_STATUS_INTERRUPT) {
            // If interrupt is requested, abort the ongoing transition.
            state_.renderer->Clear(true);
          }
        }

        LS::IOStats io_stats = LS::DriverStats();
        ESP_LOGI(TAG, "Observed %d underflows, %d near-misses", io_stats.underflow_actual,
                 io_stats.underflow_near_miss);
#if ISR_DEVELOPMENT
        ESP_LOGI(TAG, "ISR latency [%d, %d], late wakeup %d times",
                 io_stats.isr_process_latency_low / g_esp_ticks_per_us,
                 io_stats.isr_process_latency_high / g_esp_ticks_per_us, io_stats.isr_late_wakeup);
#endif
        continue;
      }
      // Nothing to do, just sleep for a bit.
      vTaskDelay(TWILIGHT_TASK_IDLE);
    }
  }

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
  xEventGroupSetBits(state_.status, TWILIGHT_STATUS_INTERRUPT);
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.strip_lock);

  // Turn off all currently configured pixels.
  ESP_RETURN_ON_ERROR(
      state_.renderer->EnqueueOrError(LS::UniformColorTarget::Create(0, LS::RGB8BPixel::BLACK())));
  state_.renderer->WaitFor(LS::RENDERER_IDLE_TARGET, portMAX_DELAY);

  ESP_RETURN_ON_ERROR(LS::DriverStop());
  ASSIGN_OR_RETURN(auto new_renderer, LS::Renderer::Create(config.num_pixels, TWILIGHT_TARGET_FPS,
                                                           TWILIGHT_RENDERER_BLEND_MODE));
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
                   LS::Renderer::Create(config_ ? config_.num_pixels : TWILIGHT_DEFAULT_PIXELS,
                                        TWILIGHT_TARGET_FPS, TWILIGHT_RENDERER_BLEND_MODE));

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

}  // namespace

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

utils::ESPErrorStatus Setup_TestTransition(Config::Transition&& transition) {
  ESP_LOGI(TAG, "Testing transition: %s", print_transition(transition).c_str());

  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (!state_.config_setup.has_value()) {
    return {"Not in setup mode"};
  }

  xEventGroupClearBits(state_.status, TWILIGHT_STATUS_SETUP_MASK);
  xEventGroupSetBits(state_.status, TWILIGHT_STATUS_SETUP_TRANSITION);
  state_.test_transition = std::move(transition);
  state_.test_countdown = TWILIGHT_SETUP_TEST_COUNTS;

  xEventGroupSetBits(state_.status, TWILIGHT_STATUS_INTERRUPT);
  return ESP_OK;
}

utils::ESPErrorStatus Set_Transitions(Config::TransitionsMap&& transitions) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (!state_.config_setup.has_value()) {
    return {"Not in setup mode"};
  }

  state_.config_setup->transitions = std::move(transitions);
#ifndef NDEBUG
  log_config(*state_.config_setup);
#endif

  return ESP_OK;
}

utils::ESPErrorStatus Set_Events(Config::EventsList&& events) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (!state_.config_setup.has_value()) {
    return {"Not in setup mode"};
  }

  state_.config_setup->events = std::move(events);
#ifndef NDEBUG
  log_config(*state_.config_setup);
#endif

  return ESP_OK;
}

utils::ESPErrorStatus Perform_Override(int32_t duration, Config::Transition&& transition) {
  ZW_ACQUIRE_FOR_SCOPE_SIMPLE(state_.state_lock);
  if (state_.config_setup.has_value()) {
    return {"In setup mode"};
  }

  ASSIGN_OR_RETURN(struct tm time_tm, time::GetLocalTime());
  int32_t second_of_day = get_second_of_day(time_tm);

  // Set override data
  state_.manual_transition = std::move(transition);
  state_.manual_override = std::make_pair(second_of_day - 1, duration);

  // Invalidate pre-computed event sequence
  state_.event_sequence.clear();

  return ESP_OK;
}

esp_err_t config_init(void) {
  ESP_LOGD(TAG, "Initializing for config...");
  return config::register_custom_field("twilight", _get_config, parse_config, log_config,
                                       marshal_config);
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
