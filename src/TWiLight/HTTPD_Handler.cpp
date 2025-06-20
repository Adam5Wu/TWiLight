#include "HTTPD_Handler.hpp"

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"

#include "ZWUtils.hpp"

#include "AppHTTPD/Interface.hpp"

#include "Interface_Private.hpp"

namespace zw::esp8266::app::twilight {
namespace {

inline constexpr char TAG[] = "TWiLight-HTTPD";

inline constexpr char URI_PATTERN[] = "/!twilight*";
#define URI_PATH_DELIM '/'

using SubFuncHandler = bool (*)(const char*, httpd_req_t*);

//----------------------
// Setup Subfunction

inline constexpr char FEATURE_SETUP_PREFIX[] = "/setup";

inline constexpr char PARAM_SETUP_STATE[] = "state";
inline constexpr char SETUP_STATE_ENTER[] = "enter";
inline constexpr char SETUP_STATE_EXIT_SAVE[] = "exit-save";
inline constexpr char SETUP_STATE_EXIT_DISCARD[] = "exit-discard";
inline constexpr char PARAM_SETUP_NUM_PIXELS[] = "num_pixels";
inline constexpr char PARAM_SETUP_TEST_TRANSITION[] = "test_transition";
inline constexpr char PARAM_SETUP_TRANSITIONS[] = "transitions";
inline constexpr char PARAM_SETUP_EVENTS[] = "events";

void _setup_getconfig(httpd_req_t* req) {
  auto config_state = GetConfigState();
  if (!config_state) {
    ESP_LOGD(TAG, "Unable to fetch config state");
    goto failed;
  }

  {
    utils::AutoReleaseRes<cJSON*> json(cJSON_CreateObject(), cJSON_Delete);
    if (*json == nullptr) {
      ESP_LOGD(TAG, "Failed to create root JSON object");
      goto failed;
    }
    if (cJSON_AddBoolToObject(*json, "setup", config_state->setup) == NULL) {
      ESP_LOGD(TAG, "Failed to add `setup` field");
      goto failed;
    }

    utils::AutoReleaseRes<cJSON*> config_json;
    if (marshal_config(config_json, Config(), config_state->config) != ESP_OK) {
      ESP_LOGD(TAG, "Failed to marshal config");
      goto failed;
    }
    if (*config_json != nullptr) {
      cJSON_AddItemReferenceToObject(*json, "config", *config_json);
    }

    httpd::send_json(req, *json);
    return;
  }

failed:
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error fetching config state");
}

void _setup_state(const std::string& state_str, httpd_req_t* req) {
  if (state_str == SETUP_STATE_ENTER) {
    if (auto result = Setup_Enter(); !result) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
      return;
    }
    httpd_resp_send(req, NULL, 0);
    return;
  }
  if (state_str == SETUP_STATE_EXIT_SAVE) {
    if (auto result = Setup_Exit(true); !result) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
      return;
    }
    httpd_resp_send(req, NULL, 0);
    return;
  }
  if (state_str == SETUP_STATE_EXIT_DISCARD) {
    if (auto result = Setup_Exit(false); !result) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
      return;
    }
    httpd_resp_send(req, NULL, 0);
    return;
  }

  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unrecognized setup state");
}

void _setup_num_pixels(const std::string& num_pixels_str, httpd_req_t* req) {
  char* endptr;
  size_t num_pixels = strtoul(num_pixels_str.data(), &endptr, 10);
  if (*endptr != '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed strip size");
    return;
  }

  auto result = Setup_StripSize(num_pixels);
  if (!result) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
    return;
  }

  httpd_resp_send(req, NULL, 0);
}

void _setup_test_transition(httpd_req_t* req) {
  utils::AutoReleaseRes<cJSON*> json;
  if (httpd::receive_json(req, json) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive transition data");
    return;
  }

  auto transition = parse_transition(*json, true);
  if (!transition) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid transition data");
    return;
  }

  auto result = Setup_TestTransition(std::move(*transition));
  if (!result) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
    return;
  }

  httpd_resp_send(req, NULL, 0);
}

void _update_transitions(httpd_req_t* req) {
  utils::AutoReleaseRes<cJSON*> json;
  if (httpd::receive_json(req, json) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive transitions data");
    return;
  }

  auto transitions = parse_transitions_map(*json, true);
  if (!transitions) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid transitions data");
    return;
  }

  auto result = Set_Transitions(std::move(*transitions));
  if (!result) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
    return;
  }

  httpd_resp_send(req, NULL, 0);
}

void _update_events(httpd_req_t* req) {
  utils::AutoReleaseRes<cJSON*> json;
  if (httpd::receive_json(req, json) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive events data");
    return;
  }

  auto events = parse_events_list(*json, true);
  if (!events) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid events data");
    return;
  }

  auto result = Set_Events(std::move(*events));
  if (!result) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
    return;
  }

  httpd_resp_send(req, NULL, 0);
}

bool _subfunc_setup(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_SETUP_PREFIX, utils::STRLEN(FEATURE_SETUP_PREFIX)) != 0)
    return false;

  const char* query_frag = feature + utils::STRLEN(FEATURE_SETUP_PREFIX);
  do {
    if (*query_frag == '\0') {
      if (req->method != HTTP_GET) goto method_not_allowed;
      _setup_getconfig(req);
      break;
    }

    auto state_str = httpd::query_parse_param(query_frag, PARAM_SETUP_STATE, 12);
    if (state_str) {
      if (req->method != HTTP_GET) goto method_not_allowed;
      _setup_state(*state_str, req);
      break;
    }

    auto num_pixels_str = httpd::query_parse_param(query_frag, PARAM_SETUP_NUM_PIXELS, 4);
    if (num_pixels_str) {
      if (req->method != HTTP_GET) goto method_not_allowed;
      _setup_num_pixels(*num_pixels_str, req);
      break;
    }

    auto test_transition_flag =
        httpd::query_parse_param(query_frag, PARAM_SETUP_TEST_TRANSITION, 0);
    if (test_transition_flag) {
      if (req->method != HTTP_POST) goto method_not_allowed;
      _setup_test_transition(req);
      break;
    }

    auto transitions_flag = httpd::query_parse_param(query_frag, PARAM_SETUP_TRANSITIONS, 0);
    if (transitions_flag) {
      if (req->method != HTTP_PUT) goto method_not_allowed;
      _update_transitions(req);
      break;
    }

    auto events_flag = httpd::query_parse_param(query_frag, PARAM_SETUP_EVENTS, 0);
    if (events_flag) {
      if (req->method != HTTP_PUT) goto method_not_allowed;
      _update_events(req);
      break;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid setup parameter");
  } while (0);
  return true;

method_not_allowed:
  httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Unexpected method");
  return true;
}

//----------------------
// Override Subfunction

inline constexpr char FEATURE_OVERRIDE_PREFIX[] = "/override";

inline constexpr char PARAM_OVERRIDE_DURATION[] = "duration";

void _handle_override(const std::string& duration_str, httpd_req_t* req) {
  int32_t duration = -1;
  if (!duration_str.empty()) {
    char* endptr;
    duration = strtoul(duration_str.data(), &endptr, 10);
    if (*endptr != '\0') {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed duration");
      return;
    }
    if (duration > SECONDS_IN_A_DAY) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Duration too long");
      return;
    }
  }

  utils::AutoReleaseRes<cJSON*> json;
  if (httpd::receive_json(req, json) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive transition data");
    return;
  }

  auto transition = parse_transition(*json, true);
  if (!transition) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid transition data");
    return;
  }

  auto result = Perform_Override(duration, std::move(*transition));
  if (!result) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
    return;
  }

  httpd_resp_send(req, NULL, 0);
}

bool _subfunc_override(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_OVERRIDE_PREFIX, utils::STRLEN(FEATURE_OVERRIDE_PREFIX)) != 0)
    return false;

  const char* query_frag = feature + utils::STRLEN(FEATURE_OVERRIDE_PREFIX);
  auto duration_str = httpd::query_parse_param(query_frag, PARAM_OVERRIDE_DURATION, 0);
  if (!duration_str) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing override duration");
    return true;
  }

  if (req->method != HTTP_POST) {
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Unexpected method");
    return true;
  }

  _handle_override(*duration_str, req);
  return true;
}

const std::vector<SubFuncHandler> subfunc_ = {_subfunc_setup, _subfunc_override};

esp_err_t _handler_twilight(httpd_req_t* req) {
  ESP_LOGI(TAG, "[%s] %s", http_method_str((enum http_method)req->method), req->uri);
  const char* feature = req->uri + utils::STRLEN(URI_PATTERN) - 1;
  if (*feature == '\0' || *feature != URI_PATH_DELIM) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed request");
  }

  for (const auto& subfunc : subfunc_) {
    if (subfunc(feature, req)) {
      return ESP_OK;
    }
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Feature not available");
}

}  // namespace

esp_err_t register_httpd_handler(httpd_handle_t httpd) {
  ESP_LOGD(TAG, "Register handler on %s", URI_PATTERN);
  httpd_uri_t handler = {
      .uri = URI_PATTERN,
      .method = HTTP_ANY,
      .handler = _handler_twilight,
      .user_ctx = NULL,
  };
  return httpd_register_uri_handler(httpd, &handler);
}

}  // namespace zw::esp8266::app::twilight
