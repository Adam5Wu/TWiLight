#include "Handler_SysFunc.hpp"

#include <string>
#include <vector>

#include "cJSON.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "AppEventMgr/Interface.hpp"
#include "AppStorage/Interface.hpp"

#include "Interface.hpp"

#ifdef ZW_APPLIANCE_COMPONENT_WEB_SYSFUNC

#ifdef ZW_APPLIANCE_COMPONENT_WEB_NET_PROVISION
#include "Handler_SysFunc_Provision.hpp"
#endif
#ifdef ZW_APPLIANCE_COMPONENT_WEB_OTA
#include "Handler_SysFunc_OTA.hpp"
#endif
#include "Handler_SysFunc_Config.hpp"

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD-SYSFUNC";

inline constexpr char URI_PATTERN[] = "/!sys*";
#define URI_PATH_DELIM '/'

using SysFuncHandler = bool (*)(const char*, httpd_req_t*);

// A boot serial is a random string generated per boot
// It is checked when the backend performs certain critical operations from Web requests.
// This prevents users accidentally trigger undesired actions based on stale information
// from an older version of the page cached in the browser.
std::string boot_serial_;

inline void append_hex_digit(uint32_t val, std::string& str) {
  for (int i = 0; i < 8; i++) {
    uint8_t digit = val & 0xF;
    str.push_back((char)(digit < 10 ? '0' + digit : 'A' + digit - 10));
    val >>= 4;
  }
}

esp_err_t init_boot_serial_() {
  if (boot_serial_.empty()) {
    append_hex_digit(esp_random(), boot_serial_);
  }
  ESP_LOGD(TAG, "Boot serial: %s", boot_serial_.c_str());
  return ESP_OK;
}

inline constexpr char FEATURE_BOOT_SERIAL[] = "/boot_serial";

bool sysfunc_boot_serial(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_BOOT_SERIAL, utils::STRLEN(FEATURE_BOOT_SERIAL)) != 0) return false;

  if (feature[utils::STRLEN(FEATURE_BOOT_SERIAL)] != '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed request");
    return true;
  }

  switch (req->method) {
    case HTTP_GET:
      httpd_resp_set_type(req, HTTPD_200);
      httpd_resp_send(req, boot_serial_.data(), boot_serial_.length());
      break;

    default:
      httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Feature does not accept this method");
  }
  return true;
}

inline constexpr char PARAM_BOOT_SERIAL[] = "bs";

bool _check_boot_serial(const char* query_frag, httpd_req_t* req) {
  auto bs_param = query_parse_param(query_frag, PARAM_BOOT_SERIAL, 8);
  if (!bs_param) {
    ESP_LOGW(TAG, "Boot serial parameter missing or invalid");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid parameter");
    return false;
  }
  if (boot_serial_.compare(*bs_param) != 0) {
    ESP_LOGW(TAG, "Unmatched boot serial, expect '%s', got '%s'", boot_serial_.c_str(),
             bs_param->c_str());
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid boot serial, stale session?");
    return false;
  }
  return true;
}

inline constexpr char FEATURE_REBOOT[] = "/reboot";

bool sysfunc_reboot(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_REBOOT, utils::STRLEN(FEATURE_REBOOT)) != 0) return false;

  if (_check_boot_serial(feature + utils::STRLEN(FEATURE_REBOOT), req)) {
    switch (req->method) {
      case HTTP_GET:
        httpd_resp_set_status(req, HTTPD_204);
        httpd_resp_send(req, NULL, 0);
        eventmgr::system_event_post(ZW_SYSTEM_EVENT_REBOOT);
        break;

      default:
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                            "Feature does not accept this method");
    }
  }
  return true;
}

inline constexpr char FEATURE_STORAGE[] = "/storage";

inline constexpr char HTTP_MIME_BINARY[] = "application/octet-stream";
inline constexpr char HTTP_HEADER_CONTENT_LENGTH[] = "Content-Length";
inline constexpr char HTTP_HEADER_CONTENT_DISPOSITION[] = "Content-Disposition";
inline constexpr char HTTP_HEADER_CONTENT_DISPOSITION_VALUE_TMPL[] =
    "attachment; filename=\"storage_%d.littlefs\"";

inline constexpr char HTTPD_409[] = "409 Conflict";

bool storage_op_in_progress_ = false;

esp_err_t _storage_dump(const char* query_frag, httpd_req_t* req) {
  if (storage_op_in_progress_) {
    httpd_resp_send_custom_err(req, HTTPD_409, "Storage operation in progress");
    return ESP_OK;
  }
  storage_op_in_progress_ = true;
  utils::AutoRelease storage_cleanup([&] { storage_op_in_progress_ = false; });

  if (*query_frag != '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected query fragment");
    return ESP_OK;
  }
  ASSIGN_OR_RETURN(auto accessor, storage::data_partition_access(false));

  char size_buf[10];
  snprintf(size_buf, 10, "%d", accessor->sectors() * SPI_FLASH_SEC_SIZE);
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, HTTP_HEADER_CONTENT_LENGTH, size_buf));
  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTP_MIME_BINARY));

  struct timeval tv;
  gettimeofday(&tv, NULL);
  utils::DataBuf filename;
  ESP_RETURN_ON_ERROR(
      httpd_resp_set_hdr(req, HTTP_HEADER_CONTENT_DISPOSITION,
                         filename.PrintTo(HTTP_HEADER_CONTENT_DISPOSITION_VALUE_TMPL, tv.tv_sec)));

  utils::DataBuf data(SPI_FLASH_SEC_SIZE);
  for (size_t idx = 0; idx < accessor->sectors(); idx++) {
    ESP_RETURN_ON_ERROR(accessor->read_sector(idx, data.data()));
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, (const char*)data.data(), SPI_FLASH_SEC_SIZE));
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t _storage_restore(httpd_req_t* req) {
  if (storage_op_in_progress_) {
    httpd_resp_send_custom_err(req, HTTPD_409, "Storage operation in progress");
    return ESP_OK;
  }
  storage_op_in_progress_ = true;
  utils::AutoRelease storage_cleanup([&] { storage_op_in_progress_ = false; });

  ASSIGN_OR_RETURN(auto accessor, storage::data_partition_access(true));
  size_t ota_data_len = req->content_len;
  if (ota_data_len > accessor->sectors() * SPI_FLASH_SEC_SIZE) {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Storage partition oversize");
    return ESP_OK;
  }
  if (ota_data_len % SPI_FLASH_SEC_SIZE != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid OTA data size");
    return ESP_OK;
  }

  utils::DataBuf ota_data(SPI_FLASH_SEC_SIZE);
  for (size_t idx = 0; idx < accessor->sectors(); idx++) {
    int read_pos = 0;
    while (read_pos < SPI_FLASH_SEC_SIZE) {
      int recv_len =
          httpd_req_recv(req, (char*)&ota_data.front() + read_pos, SPI_FLASH_SEC_SIZE - read_pos);
      if (recv_len <= 0) {
        ESP_LOGW(TAG, "Data partition read short by %d (+%d sectors)",
                 SPI_FLASH_SEC_SIZE - read_pos, accessor->sectors() - idx - 1);
        return ESP_FAIL;
      }
      read_pos += recv_len;
    }
    ESP_RETURN_ON_ERROR(accessor->write_sector(idx, ota_data.data()));
  }

  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, NULL, 0));
  return ESP_OK;
}

esp_err_t _storage_reset(httpd_req_t* req) {
  if (storage_op_in_progress_) {
    httpd_resp_send_custom_err(req, HTTPD_409, "Storage operation in progress");
    return ESP_OK;
  }
  storage_op_in_progress_ = true;
  utils::AutoRelease storage_cleanup([&] { storage_op_in_progress_ = false; });

  ASSIGN_OR_RETURN(auto accessor, storage::data_partition_access(true));
  // Erase the first five sectors
  utils::DataBuf erase_data(SPI_FLASH_SEC_SIZE);
  for (size_t idx = 0; idx < std::min(5U, accessor->sectors()); idx++) {
    ESP_RETURN_ON_ERROR(accessor->write_sector(idx, erase_data.data()));
  }
  // Note that this will leave the partition in an invalid state,
  // and will require a reboot to recover!

  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, NULL, 0));
  return ESP_OK;
}

bool sysfunc_storage(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_STORAGE, utils::STRLEN(FEATURE_STORAGE)) != 0) return false;
  const char* query_frag = feature + utils::STRLEN(FEATURE_STORAGE);

  switch (req->method) {
    case HTTP_GET:
      if (_storage_dump(query_frag, req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to dump storage");
      }
      break;

    case HTTP_PUT:
      if (_check_boot_serial(query_frag, req)) {
        if (_storage_restore(req) != ESP_OK) {
          httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restore storage");
        }
      }
      break;

    case HTTP_DELETE:
      if (_check_boot_serial(query_frag, req)) {
        if (_storage_reset(req) != ESP_OK) {
          httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset storage");
        }
      }
      break;

    default:
      httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Feature does not accept this method");
  }
  return true;
}

const std::vector<SysFuncHandler> subfunc_ = {
    sysfunc_boot_serial, sysfunc_reboot, sysfunc_storage, sysfunc_config,
#ifdef ZW_APPLIANCE_COMPONENT_WEB_NET_PROVISION
    sysfunc_provision,
#endif
#ifdef ZW_APPLIANCE_COMPONENT_WEB_OTA
    sysfunc_ota,
#endif
};

esp_err_t _handler_sysfunc(httpd_req_t* req) {
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

esp_err_t register_handler_sysfunc(httpd_handle_t httpd) {
  ESP_RETURN_ON_ERROR(init_boot_serial_());

  ESP_LOGD(TAG, "Register handler on %s", URI_PATTERN);
  {
    httpd_uri_t handler = {
        .uri = URI_PATTERN,
        .method = HTTP_ANY,
        .handler = _handler_sysfunc,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(httpd, &handler));
  }
  return ESP_OK;
}

}  // namespace zw::esp8266::app::httpd

#endif  // ZW_APPLIANCE_COMPONENT_WEB_SYSFUNC