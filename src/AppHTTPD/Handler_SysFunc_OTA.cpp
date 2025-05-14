#include "Handler_SysFunc_OTA.hpp"

#include <stdlib.h>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spi_flash.h"

#include "lwip/sockets.h"

#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "AppConfig/Interface.hpp"
#include "AppEventMgr/Interface.hpp"

#include "Interface.hpp"

#ifdef ZW_APPLIANCE_COMPONENT_WEB_OTA

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD-OTA";

inline constexpr char FEATURE_PREFIX[] = "/ota";
#define URI_PATH_DELIM '/'

inline constexpr char OTA_STATE[] = "/state";
inline constexpr char OTA_DATA[] = "/data";
inline constexpr char OTA_TOGGLE[] = "/toggle";

// Default netmask to use if config is missing or malformed
// For IPv4: 255.255.255.0 --> little endian 0x00FFFFFF
inline constexpr ip_addr_t DEFAULT_NETMASK = {.addr = 0x00FFFFFF};

using config::AppConfig;

bool ota_in_progress_ = false;

const char* _ota_desc_gen_data(int index, const esp_app_desc_t& desc, utils::DataBuf& format_buf) {
  return format_buf.PrintTo(
      R"json("index":%d,"image_name":"%s","image_ver":"%s","build_time":"%s %s","idf_ver":"%s")json",
      index, desc.project_name, desc.version, desc.date, desc.time, desc.idf_ver);
}

const char* _ota_part_gen_data(const esp_partition_t* ota_part, utils::DataBuf& format_buf) {
  int index = ota_part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;

  esp_app_desc_t partdesc;
  if (esp_err_t err = esp_ota_get_partition_description(ota_part, &partdesc); err == ESP_OK) {
    return _ota_desc_gen_data(index, partdesc, format_buf);
  } else {
    ESP_LOGD(TAG, "OTA partition #%d does not have a descriptor", index);
    return format_buf.PrintTo(R"json("index":%d)json", index);
  }
}

esp_err_t _ota_state_gen_data(std::string& data) {
  const esp_partition_t* boot_part = esp_ota_get_boot_partition();
  if (boot_part == NULL) return ESP_ERR_NOT_SUPPORTED;
  const esp_partition_t* cur_part = esp_ota_get_running_partition();
  if (cur_part == NULL) return ESP_ERR_NOT_SUPPORTED;

  utils::DataBuf ota_entry;
  data.append("\n {")
      .append(_ota_part_gen_data(cur_part, ota_entry))
      .append(cur_part == boot_part ? R"json(,"next":true)json" : "")
      .append("},");

  const esp_partition_t* ota_part = boot_part;
  if (cur_part == boot_part) {
    ota_part = esp_ota_get_next_update_partition(cur_part);
    if (ota_part == NULL) return ESP_ERR_NOT_SUPPORTED;
  }
  data.append("\n {")
      .append(_ota_part_gen_data(ota_part, ota_entry))
      .append(ota_part == boot_part ? R"json(,"next":true)json" : "")
      .append("}");

  return ESP_OK;
}

esp_err_t _ota_state(httpd_req_t* req) {
  std::string ota_state_json = "[";
  if (_ota_state_gen_data(ota_state_json) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Failed to generate OTA state data");
  }
  ota_state_json.append("\n]");

  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTPD_TYPE_JSON));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, ota_state_json.data(), ota_state_json.length()));
  return ESP_OK;
}

esp_err_t _ota_data(httpd_req_t* req) {
  const esp_partition_t* ota_part = esp_ota_get_next_update_partition(NULL);
  if (ota_part == NULL) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Failed to get OTA update partition");
  }

  size_t ota_data_len = req->content_len;
  if (ota_data_len <=
      sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid OTA data size");
  } else if (ota_data_len > ota_part->size) {
    return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "OTA data oversize");
  }

  esp_ota_handle_t ota_handle;
  if (esp_ota_begin(ota_part, ota_data_len, &ota_handle) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start OTA");
  }

  {
    ota_in_progress_ = true;
    utils::AutoRelease ota_cleanup([&] {
      ota_in_progress_ = false;
      if (esp_ota_end_ex(ota_handle, true) == ESP_OK) {
        eventmgr::system_event_post(ZW_SYSTEM_EVENT_BOOT_IMAGE_ALT);
        eventmgr::system_states_set(ZW_SYSTEM_STATE_BOOT_IMAGE_ALT);
      } else {
        ESP_LOGW(TAG, "Failed to finalize OTA");
      }
    });

    utils::DataBuf ota_data(SPI_FLASH_SEC_SIZE);
    while (ota_data_len > 0) {
      int recv_len = httpd_req_recv(req, (char*)&ota_data.front(), SPI_FLASH_SEC_SIZE);
      if (recv_len <= 0) break;
      ota_data_len -= recv_len;
      if (esp_ota_write(ota_handle, &ota_data.front(), recv_len) == ESP_OK) continue;
    }
    if (ota_data_len > 0) {
      ESP_LOGW(TAG, "Post receive short by %d", ota_data_len);
    }
  }

  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  ESP_RETURN_ON_ERROR(httpd_resp_send(req, NULL, 0));
  return ESP_OK;
}

utils::DataOrError<const esp_partition_t*> _ota_toggle_target() {
  const esp_partition_t* boot_part = esp_ota_get_boot_partition();
  if (boot_part == NULL) return ESP_ERR_NOT_SUPPORTED;
  const esp_partition_t* cur_part = esp_ota_get_running_partition();
  if (cur_part == NULL) return ESP_ERR_NOT_SUPPORTED;

  if (cur_part != boot_part) {
    return cur_part;
  }

  const esp_partition_t* next_part = esp_ota_get_next_update_partition(cur_part);
  if (next_part == NULL) return ESP_ERR_NOT_SUPPORTED;
  return next_part;
}

inline constexpr char PARAM_INDEX[] = "index";
inline constexpr char HTTPD_409[] = "409 Conflict";

esp_err_t _ota_toggle(const char* query_str, httpd_req_t* req) {
  auto index_param = query_parse_param(query_str, PARAM_INDEX, 2);
  if (!index_param) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid parameter");
  }
  char* endptr;
  int index = strtol(index_param->data(), &endptr, 10);
  if (*endptr != '\0') {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed partition index");
  }
  if (index != 0 && index != 1) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid partition index");
  }

  auto toggle_target = _ota_toggle_target();
  if (!toggle_target) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Unable to fetch OTA partition info");
  }

  if ((*toggle_target)->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_MIN + index) {
    return httpd_resp_send_custom_err(req, HTTPD_409, "Unexpected target boot partition");
  }

  if (esp_ota_set_boot_partition(*toggle_target) != ESP_OK) {
    return httpd_resp_send_custom_err(req, HTTPD_500, "Failed to toggle boot partition");
  }
  eventmgr::system_event_post(ZW_SYSTEM_EVENT_BOOT_IMAGE_ALT);
  eventmgr::system_states_set(ZW_SYSTEM_STATE_BOOT_IMAGE_ALT);

  ESP_RETURN_ON_ERROR(httpd_resp_send(req, NULL, 0));
  return ESP_OK;
}

esp_err_t _get_connection_ips(httpd_req_t* req, sockaddr_in* server_addr,
                              sockaddr_in* client_addr) {
  int s = httpd_req_to_sockfd(req);
  {
    socklen_t addrlen = sizeof(*server_addr);
    if (lwip_getsockname(s, (struct sockaddr*)server_addr, &addrlen) != 0) {
      ESP_LOGW(TAG, "Failed to get server IP address");
      return ESP_FAIL;
    }
  }
  {
    socklen_t addrlen = sizeof(*client_addr);
    if (lwip_getpeername(s, (struct sockaddr*)client_addr, &addrlen) != 0) {
      ESP_LOGW(TAG, "Failed to get client IP address");
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

esp_err_t _handler_ota(const char* feature, httpd_req_t* req) {
  if (*feature == '\0' || *feature != URI_PATH_DELIM) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed request");
  }

  // Ensure clients are in allowed network
  {
    auto config = config::get()->http_server.web_ota;

    sockaddr_in server_addr, client_addr;
    if (_get_connection_ips(req, &server_addr, &client_addr) != ESP_OK) {
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "Failed to get connection IP addresses");
    }
    if (server_addr.sin_family != AF_INET || client_addr.sin_family != AF_INET) {
      return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Connection is not IPv4");
    }
    ip4_addr_t server_ip = {.addr = server_addr.sin_addr.s_addr};
    ip4_addr_t client_ip = {.addr = client_addr.sin_addr.s_addr};
    ip4_addr_t netmask = config.netmask.value_or(DEFAULT_NETMASK);
    if (!ip_addr_netcmp(&server_ip, &client_ip, &netmask)) {
      return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Client IP not acceptable");
    }
  }

  if (ota_in_progress_) {
    return httpd_resp_send_custom_err(req, HTTPD_409, "OTA in progress");
  }

  if (feature) {
    if (strcmp(feature, OTA_STATE) == 0) {
      switch (req->method) {
        case HTTP_GET:
          return _ota_state(req);
        default:
          goto method_not_allowed;
      }
    } else if (strcmp(feature, OTA_DATA) == 0) {
      switch (req->method) {
        case HTTP_POST:
          return _ota_data(req);
        default:
          goto method_not_allowed;
      }
    } else if (strncmp(feature, OTA_TOGGLE, utils::STRLEN(OTA_TOGGLE)) == 0) {
      switch (req->method) {
        case HTTP_GET:
          return _ota_toggle(feature + utils::STRLEN(OTA_TOGGLE), req);
        default:
          goto method_not_allowed;
      }
    }
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Feature not available");

method_not_allowed:
  return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                             "Feature does not accept this method");
}

}  // namespace

bool sysfunc_ota(const char* feature, httpd_req_t* req) {
  if (strncmp(feature, FEATURE_PREFIX, utils::STRLEN(FEATURE_PREFIX)) != 0) return false;

  if (esp_err_t err = _handler_ota(feature + utils::STRLEN(FEATURE_PREFIX), req); err != ESP_OK) {
    ESP_LOGW(TAG, "OTA request handler error: %d (0x%x)", err, err);
  }
  return true;
}

}  // namespace zw::esp8266::app::httpd

#endif  // ZW_APPLIANCE_COMPONENT_WEB_OTA