#include "HTTPD_Handler_TWiLight.hpp"

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"

#include "ZWUtils.hpp"
#include "ZWAppUtils.hpp"

#include "AppConfig/Interface.hpp"
#include "AppHTTPD/Interface.hpp"

#include "Interface.hpp"

namespace zw::esp8266::app::twilight {
namespace {

inline constexpr char TAG[] = "TWiLight-HTTPD";

inline constexpr char URI_PATTERN[] = "/!twilight*";
#define URI_PATH_DELIM '/'

using config::AppConfig;

using SubFuncHandler = bool (*)(const char*, httpd_req_t*);

inline constexpr char FEATURE_SETUP_PREFIX[] = "/setup";

inline constexpr char PARAM_SETUP_STATE[] = "state";
inline constexpr char SETUP_STATE_ENTER[] = "enter";
inline constexpr char SETUP_STATE_EXIT_SAVE[] = "exit-save";
inline constexpr char SETUP_STATE_EXIT_DISCARD[] = "exit-discard";
inline constexpr char PARAM_SETUP_NUM_PIXELS[] = "num_pixels";
inline constexpr char PARAM_SETUP_TEST_TRANSITION[] = "test_transition";

void _setup_getconfig(httpd_req_t* req) {
  auto config_state = GetConfigState();
  if (!config_state) {
    ESP_LOGD(TAG, "Unable to fetch config state");
    goto failed;
  }

  {
    utils::AutoReleaseRes<cJSON*> json(cJSON_CreateObject(), [](cJSON* data) {
      if (data) cJSON_Delete(data);
    });
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

    utils::AutoReleaseRes<char*> config_str(cJSON_Print(*json), [](char* data) {
      if (data) cJSON_free(data);
    });
    if (*config_str == nullptr) {
      ESP_LOGD(TAG, "Failed to print JSON data");
      goto failed;
    }

    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_send(req, *config_str, strlen(*config_str));
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

void _setup_test_transition(const std::string& test_transition_str, httpd_req_t* req) {
  auto transition_json = utils::UrlDecode(test_transition_str);
  if (!transition_json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed transition data parameter");
    return;
  }

  Config::Transition transition;
  {
    utils::AutoReleaseRes<cJSON*> json(cJSON_Parse(transition_json->c_str()), [](cJSON* data) {
      if (data) cJSON_Delete(data);
    });
    if (*json == nullptr) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed transition data JSON");
      return;
    }

    if (parse_transition(*json, transition, true) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid transition data");
      return;
    }
    log_transition(transition);
  }

  {
    auto result = Setup_TestTransition(transition);
    if (!result) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, result.message.c_str());
      return;
    }
  }

  httpd_resp_send(req, NULL, 0);
}

bool _subfunc_setup(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_SETUP_PREFIX, utils::STRLEN(FEATURE_SETUP_PREFIX)) != 0)
    return false;

  const char* query_frag = feature + utils::STRLEN(FEATURE_SETUP_PREFIX);
  do {
    if (*query_frag == '\0') {
      _setup_getconfig(req);
      break;
    }

    auto state_str = httpd::query_parse_param(query_frag, PARAM_SETUP_STATE, 12);
    if (state_str) {
      _setup_state(*state_str, req);
      break;
    }

    auto num_pixels_str = httpd::query_parse_param(query_frag, PARAM_SETUP_NUM_PIXELS, 4);
    if (num_pixels_str) {
      _setup_num_pixels(*num_pixels_str, req);
      break;
    }

    auto test_transition_str = httpd::query_parse_param(query_frag, PARAM_SETUP_TEST_TRANSITION, 0);
    if (test_transition_str) {
      _setup_test_transition(*test_transition_str, req);
      break;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid setup parameter");
  } while (0);

  return true;
}

const std::vector<SubFuncHandler> subfunc_ = {_subfunc_setup};

esp_err_t _handler_twilight(httpd_req_t* req) {
  ESP_LOGI(TAG, "%s", req->uri);
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
  ESP_LOGI(TAG, "Register handler on %s", URI_PATTERN);
  httpd_uri_t handler = {
      .uri = URI_PATTERN,
      .method = HTTP_GET,
      .handler = _handler_twilight,
      .user_ctx = NULL,
  };
  return httpd_register_uri_handler(httpd, &handler);
}

}  // namespace zw::esp8266::app::twilight