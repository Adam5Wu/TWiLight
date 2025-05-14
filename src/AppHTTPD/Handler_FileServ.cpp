#include "Handler_FileServ.hpp"

#include <string>
#include <unordered_map>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_http_server.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "AppNetwork/Interface.hpp"

#include "Interface.hpp"
#include "Interface_Private.hpp"

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD-FS";

inline constexpr char URI_PATTERN[] = "/*";
#define URI_PATH_DELIM '/'
#define URI_EXT_DELIM '.'

inline constexpr char URI_DEFAULT_FILENAME[] = "index.html";
inline constexpr char URI_SCHEME_SEP[] = "http://";

inline constexpr char HTTP_STATUS_302_FOUND[] = "302 Found";
inline constexpr char HTTP_STATUS_304_NOT_MODIFIED[] = "304 Not Modified";

inline constexpr char HTTP_HEADER_HOST[] = "Host";
inline constexpr char HTTP_HEADER_CACHE_CONTROL[] = "Cache-Control";
inline constexpr char HTTP_HEADER_LOCATION[] = "Location";
inline constexpr char HTTP_HEADER_ETAG[] = "ETag";
inline constexpr char HTTP_HEADER_IF_NONE_MATCH[] = "If-None-Match";

inline constexpr char HTTP_CACHE_CONTROL_VALUE[] = "max-age=0, must-revalidate";

inline constexpr char HTTP_MIME_BINARY[] = "application/octet-stream";
inline constexpr char HTTP_MIME_TEXT[] = "text/plain";
inline constexpr char HTTP_MIME_CSS[] = "text/css";
inline constexpr char HTTP_MIME_CSV[] = "text/csv";
inline constexpr char HTTP_MIME_HTML[] = "text/html";
inline constexpr char HTTP_MIME_MARKDOWN[] = "text/markdown";
inline constexpr char HTTP_MIME_JAVASCRIPT[] = "text/javascript";
inline constexpr char HTTP_MIME_JPEG[] = "image/jpeg";
inline constexpr char HTTP_MIME_GIF[] = "image/gif";
inline constexpr char HTTP_MIME_PNG[] = "image/png";
inline constexpr char HTTP_MIME_ICON[] = "image/vnd.microsoft.icon";
inline constexpr char HTTP_MIME_JSON[] = "application/json";
inline constexpr char HTTP_MIME_ZIP[] = "application/zip";
inline constexpr char HTTP_MIME_MP3[] = "audio/mpeg";
inline constexpr char HTTP_MIME_AAC[] = "audio/aac";
inline constexpr char HTTP_MIME_MIDI[] = "audio/midi";
inline constexpr char HTTP_MIME_XML[] = "application/xml";
inline constexpr char HTTP_MIME_XHTML[] = "application/xhtml+xml";

#define INFER_TYPE_FROM_EXT(ext, type, type_ext) \
  if (strcmp(ext, type_ext) == 0) return HTTP_MIME_##type

#define INFER_TYPE_FROM_2EXT(ext, type, type_ext1, type_ext2) \
  if (strcmp(ext, type_ext1) == 0 || strcmp(ext, type_ext2) == 0) return HTTP_MIME_##type

const char* _uri_infer_mimetype(const char* uri) {
  const char* ext = strrchr(uri, URI_EXT_DELIM);
  if (ext) {
    INFER_TYPE_FROM_EXT(ext, TEXT, ".txt");
    INFER_TYPE_FROM_EXT(ext, CSS, ".css");
    INFER_TYPE_FROM_EXT(ext, CSV, ".csv");
    INFER_TYPE_FROM_2EXT(ext, HTML, ".htm", ".html");
    INFER_TYPE_FROM_EXT(ext, MARKDOWN, ".md");
    INFER_TYPE_FROM_EXT(ext, JAVASCRIPT, ".js");
    INFER_TYPE_FROM_2EXT(ext, JPEG, ".jpg", ".jpeg");
    INFER_TYPE_FROM_EXT(ext, GIF, ".gif");
    INFER_TYPE_FROM_EXT(ext, PNG, ".png");
    INFER_TYPE_FROM_EXT(ext, ICON, ".ico");
    INFER_TYPE_FROM_EXT(ext, JSON, ".json");
    INFER_TYPE_FROM_EXT(ext, ZIP, ".zip");
    INFER_TYPE_FROM_EXT(ext, MP3, ".mp3");
    INFER_TYPE_FROM_EXT(ext, AAC, ".aac");
    INFER_TYPE_FROM_2EXT(ext, MIDI, ".mid", ".midi");
    INFER_TYPE_FROM_EXT(ext, XML, ".xml");
    INFER_TYPE_FROM_EXT(ext, XHTML, ".xhtml");
  }
  return HTTP_MIME_BINARY;
}

#ifdef ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS

esp_err_t _captive_redirect(httpd_req_t* req, bool& redirected) {
  std::string host(httpd_req_get_hdr_value_len(req, HTTP_HEADER_HOST), '\0');
  if (host.empty()) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No host header provided");
  }

  ESP_RETURN_ON_ERROR(
      httpd_req_get_hdr_value_str(req, HTTP_HEADER_HOST, &host.front(), host.length() + 1));
  if (strcasecmp(host.c_str(), network::Hostname().c_str()) != 0) {
    redirected = true;
    std::string redir_loc;
    redir_loc.append(URI_SCHEME_SEP).append(network::Hostname()).push_back(URI_PATH_DELIM);
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTP_STATUS_302_FOUND));
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, HTTP_HEADER_LOCATION, redir_loc.c_str()));
    return httpd_resp_send(req, NULL, 0);
  }
  return ESP_OK;
}

#endif  // ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS

esp_err_t _handler_fileserv(httpd_req_t* req) {
#ifdef ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS
  if (serving_config().provisioning) {
    bool redirected = false;
    ESP_RETURN_ON_ERROR(_captive_redirect(req, redirected));
    if (redirected) return ESP_OK;
  }
#endif

  const auto& httpd_config_ = serving_config().httpd;
  if (httpd_config_.root_dir.empty()) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "HTTP service `root_dir` not configured");
  }

  std::string file_path;
  const char* mime_type;
  {
    std::string uri = req->uri;
    if (uri.empty()) {
      uri += URI_PATH_DELIM;
    } else if (uri.front() != URI_PATH_DELIM) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URI must start with root delimiter");
    }

    if (uri.length() == 1 && serving_config().provisioning && httpd_config_.net_provision &&
        !httpd_config_.net_provision.default_page.empty()) {
      ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTP_STATUS_302_FOUND));
      ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, HTTP_HEADER_LOCATION,
                                             httpd_config_.net_provision.default_page.c_str()));
      return httpd_resp_send(req, NULL, 0);
    }

    if (uri.back() == URI_PATH_DELIM) uri.append(URI_DEFAULT_FILENAME);
    file_path = httpd_config_.root_dir + uri;
    mime_type = _uri_infer_mimetype(uri.c_str());
  }
  ESP_LOGI(TAG, "%s -> %s", req->uri, file_path.c_str());

  utils::AutoReleaseRes<FILE*> file(fopen(file_path.c_str(), "r"), [](FILE* file) {
    if (file) fclose(file);
  });
  if (*file == NULL) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unable to open file");
  }
  struct stat st;
  if (fstat(fileno(*file), &st) != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to query file stat");
  }
  utils::DataBuf etag_buf(16);
  const char* etag = etag_buf.PrintTo("%06lX:%08lX", st.st_size & 0xffffff, st.st_mtime);

  do {
    std::string check_etag(httpd_req_get_hdr_value_len(req, HTTP_HEADER_IF_NONE_MATCH), '\0');
    if (!check_etag.empty()) {
      ZW_BREAK_ON_ERROR_SIMPLE(httpd_req_get_hdr_value_str(
          req, HTTP_HEADER_IF_NONE_MATCH, &check_etag.front(), check_etag.length() + 1));
      if (check_etag == etag) {
        ZW_BREAK_ON_ERROR_SIMPLE(httpd_resp_set_status(req, HTTP_STATUS_304_NOT_MODIFIED));
        return httpd_resp_send(req, NULL, 0);
      }
    }
  } while (0);

  ESP_LOGD(TAG, "Serving %ld bytes (%s)...", st.st_size, mime_type);
  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, mime_type));
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, HTTP_HEADER_CACHE_CONTROL, HTTP_CACHE_CONTROL_VALUE));
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, HTTP_HEADER_ETAG, etag));
  return send_file(req, *file, st.st_size);
}

}  // namespace

esp_err_t register_handler_fileserv(httpd_handle_t httpd) {
  ESP_LOGD(TAG, "Register handler on %s", URI_PATTERN);
  httpd_uri_t handler = {
      .uri = URI_PATTERN,
      .method = HTTP_GET,
      .handler = _handler_fileserv,
      .user_ctx = NULL,
  };
  return httpd_register_uri_handler(httpd, &handler);
}

}  // namespace zw::esp8266::app::httpd
