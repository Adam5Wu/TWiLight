#include <string>

#include "cJSON.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

namespace zw::esp8266::app::httpd {

inline constexpr char TAG[] = "HTTPD-UTILS";

utils::DataOrError<std::string> query_parse_param(const char* query_frag, const char* name,
                                                  size_t expect_len) {
  if (*query_frag != '?') {
    ESP_LOGD(TAG, "Not a query fragment: '%s'", query_frag);
    return ESP_ERR_NOT_FOUND;
  }

  std::string result;
  if (expect_len == 0) {
    if (esp_err_t result = httpd_query_key_value(query_frag + 1, name, NULL, &expect_len);
        result != ESP_ERR_HTTPD_RESULT_TRUNC) {
      return result;
    }
    // Exclude the null-terminator.
    --expect_len;
  }
  result.resize(expect_len);

  // Include space for null-terminator for the ESP API.
  ++expect_len;
  ESP_RETURN_ON_ERROR(httpd_query_key_value(query_frag + 1, name, &result.front(), &expect_len));
  result.resize(expect_len - 1);
  ESP_LOGD(TAG, "Query [%s] = '%s' (%d)", name, result.c_str(), expect_len);
  return result;
}

inline constexpr char HTTP_HEADER_CONTENT_LENGTH[] = "Content-Length";

esp_err_t send_file(httpd_req_t* req, FILE* f, size_t size) {
  char size_buf[10];
  snprintf(size_buf, 10, "%d", size);
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, HTTP_HEADER_CONTENT_LENGTH, size_buf));

  char buf[1024];
  while (size_t read_len = fread(buf, 1, sizeof(buf), f)) {
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, buf, read_len));
    size -= read_len;
  }
  if (size) ESP_LOGW(TAG, "File read short by %d bytes", size);
  return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t send_json(httpd_req_t* req, const cJSON* json) {
  utils::AutoReleaseRes<char*> json_str(cJSON_Print(json), [](char* data) {
    if (data) cJSON_free(data);
  });
  if (*json_str == nullptr) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to print JSON data");
  }

  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTPD_TYPE_JSON));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, *json_str, HTTPD_RESP_USE_STRLEN));
  return ESP_OK;
}

esp_err_t receive_json(httpd_req_t* req, utils::AutoReleaseRes<cJSON*>& json) {
  std::string config_str(req->content_len, '\0');
  if (int recv_len = httpd_req_recv(req, &config_str.front(), config_str.length() + 1);
      recv_len != req->content_len) {
    ESP_LOGW(TAG, "Receive short, expect %d, got %d", req->content_len, recv_len);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not all data received");
  }
  // Skip checking "Content-Type", just try parse as JSON.
  json = utils::AutoReleaseRes<cJSON*>(cJSON_ParseWithOpts(config_str.data(), NULL, true),
                                       [](cJSON* data) {
                                         if (data) cJSON_Delete(data);
                                       });
  if (*json == nullptr) {
    ESP_LOGW(TAG, "Failed to parse data (around byte %d)", cJSON_GetErrorPtr() - config_str.data());
    ESP_LOG_BUFFER_HEXDUMP(TAG, cJSON_GetErrorPtr() - 16, 32, ESP_LOG_DEBUG);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload failed to parse as JSON");
  }
  return ESP_OK;
}

}  // namespace zw::esp8266::app::httpd