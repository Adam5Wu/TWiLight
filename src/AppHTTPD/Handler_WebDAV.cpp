#include "Handler_WebDAV.hpp"

#include <string>
#include <optional>
#include <functional>
#include <initializer_list>
#include <algorithm>
#include <utility>
#include <sys/stat.h>

#include "_gcc_fs/filesystem.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_spi_flash.h"

#include "esp_http_server.h"
#include "http_parser.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface_Private.hpp"

#ifdef ZW_APPLIANCE_COMPONENT_WEBDAV

// Set this flag to always return 201 for successful PUT.
// #define DAV_COMPAT_PUT_ALWAYS_201 1
// Set this flag if ESP-IDF supports vfsstat().
// #define __ESP_FS_HAS_VFSSTAT

namespace zw::esp8266::app::httpd {
namespace {

inline constexpr char TAG[] = "HTTPD-DAV";

inline constexpr char HTTP_DATE_TMPL[] = "%a, %d %b %Y %H:%M:%S %Z";
inline constexpr char HTTP_MIME_BINARY[] = "application/octet-stream";
inline constexpr char HTTP_MIME_TEXT[] = "text/plain";
inline constexpr char HTTP_MIME_HTML[] = "text/html";

inline constexpr char URI_SCHEME_SEP[] = "http://";
#define _URI_PATTERN_ROOT "/.fs"
inline constexpr char URI_PATTERN_ROOT[] = _URI_PATTERN_ROOT;
inline constexpr char URI_PATTERN[] = _URI_PATTERN_ROOT "*";
#define URI_PATH_DELIM '/'

inline constexpr char DAV_HEADER_HOST[] = "Host";
inline constexpr char DAV_HEADER_DESTINATION[] = "Destination";
inline constexpr char DAV_HEADER_OVERWRITE[] = "Overwrite";
inline constexpr char DAV_HEADER_DEPTH[] = "Depth";

inline constexpr char DAV_HEADER_CACHE_CONTROL[] = "Cache-Control";
inline constexpr char DAV_CACHE_CONTROL_VALUE[] = "no-cache";

inline constexpr char DAV_HEADER_DAV[] = "DAV";
inline constexpr char DAV_HEADER_ACCEPT_RANGES[] = "Accept-Ranges";
inline constexpr char DAV_HEADER_CONTENT_LENGTH[] = "Content-Length";
inline constexpr char DAV_HEADER_LAST_MODIFIED[] = "Last-Modified";
inline constexpr char DAV_HEADER_LOCATION[] = "Location";
inline constexpr char DAV_HEADER_ALLOW[] = "Allow";
inline constexpr char DAV_HEADER_ETAG[] = "ETag";

#define DAV_DEPTH_INFINITE -1

inline constexpr char DAV_STATUS_201_CREATED[] = "201 Created";
inline constexpr char DAV_STATUS_302_FOUND[] = "302 Found";
inline constexpr char DAV_STATUS_409_CONFLICT[] = "409 Conflict";
inline constexpr char DAV_STATUS_412_PRECONDITION_FAILED[] = "412 Precondition Failed";
inline constexpr char DAV_STATUS_415_UNSUPPORTED_MEDIA_TYPE[] = "415 Unsupported Media Type";

inline constexpr char DAV_HTML_RESP_HEADER_TMPL[] =
    "<!DOCTYPE html><html><head><title>%s</title>%s</head>";

inline constexpr char DAV_XML_RESP_TYPE[] = "application/xml";
inline constexpr char DAV_XML_RESP_PREAMBLE[] = "<?xml version=\"1.0\"?>";
inline constexpr char DAV_XML_RESP_ERROR_TMPL[] =
    "<error xmlns=\"DAV:\"><%s><href>%s</href></%s></error>";
inline constexpr char DAV_MULTISTAT_PREAMBLE[] = "<multistatus xmlns=\"DAV:\">";
inline constexpr char DAV_MULTISTAT_POSTAMBLE[] = "</multistatus>";

inline constexpr char DAV_TAG_INFINITE_DEPTH[] = "propfind-finite-depth";

namespace FS = std::filesystem;

using FS::copy_options;
using std::chrono::system_clock;

esp_err_t _COMMON_FS_HEADERS(const struct stat& st, httpd_req_t* req, utils::DataBufStash& vcache) {
  ESP_RETURN_ON_ERROR(vcache.AllocAndPrep(32, [&](utils::DataBuf& buf) {
    struct tm lt;
    // All HTTP date/time stamps MUST be represented in Greenwich Mean Time (GMT), without exception.
    strftime((char*)&buf.front(), 32, HTTP_DATE_TMPL, gmtime_r(&st.st_mtime, &lt));
    return httpd_resp_set_hdr(req, DAV_HEADER_LAST_MODIFIED, (char*)&buf.front());
  }));
  ESP_RETURN_ON_ERROR(vcache.AllocAndPrep(20, [&](utils::DataBuf& buf) {
    return httpd_resp_set_hdr(req, DAV_HEADER_ETAG,
                              buf.PrintTo("%06lX:%08lX", st.st_size & 0xffffff, st.st_mtime));
  }));
  return ESP_OK;
}

// Normalize path and make sure it does not point to an empty filename
FS::path _NORMALIZE_PATH(const FS::path& path) {
  FS::path norm_path = path.lexically_normal();
  return norm_path.filename().empty() ? norm_path.parent_path() : norm_path;
}

const char* _DAV_XML_ERROR(utils::DataBuf& buf, const char* element, const char* href) {
  return buf.PrintTo(DAV_XML_RESP_ERROR_TMPL, element, href, element);
}

// State of a parsed value
enum class PVState { ABSENT, INVALID, PARSED };

// Contains parsed value
template <class Type>
class PValue {
 public:
  PValue(PVState st = PVState::ABSENT) : st_(st) {
#ifndef NDEBUG
    if (st == PVState::PARSED) ESP_LOGE(TAG, "!! Should not construct with PARSED state !!");
#endif
  }
  PValue(const Type& val) : st_(PVState::PARSED), val_(val) {}
  PValue(Type&& val) : st_(PVState::PARSED), val_(std::move(val)) {}

  void reset(void) { st_ = PVState::ABSENT; }
  void invalidate(void) { st_ = PVState::INVALID; }

  PVState state(void) const { return st_; }
  bool has_value(void) const { return st_ == PVState::PARSED; }
  operator bool() const { return has_value(); }
  bool state_in(std::initializer_list<PVState> l) const {
    return std::find(l.begin(), l.end(), st_) != l.end();
  }

  PValue& operator=(const Type& val) const {
    val_ = val;
    st_ = PVState::PARSED;
    return *this;
  }
  PValue& operator=(const Type&& val) const {
    val_ = std::move(val);
    st_ = PVState::PARSED;
    return *this;
  }

#ifndef NDEBUG
#define RETURN_VALUE_REF(ref)                               \
  if (!has_value()) ESP_LOGE(TAG, "!! Value is absent !!"); \
  return ref;
#else
#define RETURN_VALUE_REF(ref) return ref;
#endif

  Type& operator*(void) { RETURN_VALUE_REF(val_); }
  const Type& operator*(void) const { RETURN_VALUE_REF(val_); }
  Type* operator->(void) { RETURN_VALUE_REF(&val_); }
  const Type* operator->(void) const { RETURN_VALUE_REF(&val_); }

  const Type& value_or(const Type& def) const { return has_value() ? val_ : def; }
  bool value_in(std::initializer_list<Type> l) const {
    return has_value() ? std::find(l.begin(), l.end(), val_) != l.end() : false;
  }

 protected:
  PVState st_;
  Type val_;
};

class DAVHandler {
 public:
  static std::optional<DAVHandler> Create(httpd_req_t* req) {
    FS::path fs_path;
    {
      FS::path raw_path("/");
      // Parse and prepare file path
      const char* path_frag = req->uri + utils::STRLEN(URI_PATTERN_ROOT);
      if (*path_frag != '\0' && *path_frag != URI_PATH_DELIM) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request path");
        return std::nullopt;
      }
      if (*path_frag != '\0') raw_path /= path_frag + 1;
      fs_path = _NORMALIZE_PATH(raw_path);
      ESP_LOGI(TAG, "[%s] %s -> %s", http_method_str((enum http_method)req->method), req->uri,
               fs_path.c_str());
    }
    if (fs_path.is_relative()) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path out-of-range");
      return std::nullopt;
    }

    // Get common headers
    std::string host_prefix;
    {
      std::string host = GetHeader(DAV_HEADER_HOST, req);
      if (host.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing host header");
        return std::nullopt;
      }
      host_prefix.reserve(utils::STRLEN(URI_SCHEME_SEP) + host.length());
      host_prefix.append(URI_SCHEME_SEP).append(host);
    }

    // Create the serving object
    return DAVHandler(req, std::move(fs_path), std::move(host_prefix));
  }

  static std::string GetHeader(const char* name, httpd_req_t* req) {
    std::string value(httpd_req_get_hdr_value_len(req, name), ' ');
    do {
      if (!value.empty()) {
        ZW_BREAK_ON_ERROR(
            httpd_req_get_hdr_value_str(req, name, &value.front(), value.length() + 1),
            ESP_LOGD(TAG, "[H] %s = '%s'", name, value.c_str()), ESP_LOGD(TAG, "[!H] %s", name);
            value.clear())
      }
    } while (0);
    return value;
  }

  esp_err_t Run(void);

 protected:
  DAVHandler(httpd_req_t* req, FS::path&& fs_path, std::string&& host_prefix)
      : req_(req), fs_path_(std::move(fs_path)), host_prefix_(std::move(host_prefix)) {}

  inline std::string _GetHeader(const char* name) { return GetHeader(name, req_); }

  PValue<bool> _GetHeaderBool(const char* name) {
    std::string val = _GetHeader(name);
    if (val == "T") return true;
    if (val == "F") return false;
    return val.empty() ? PVState::ABSENT : PVState::INVALID;
  }
  inline PValue<bool> _GetHeaderOverwrite(void) { return _GetHeaderBool(DAV_HEADER_OVERWRITE); }

  PValue<int8_t> _GetHeaderSmallNum(const char* name) {
    std::string val = _GetHeader(name);
    if (val == "0") return 0;
    if (val == "1") return 1;
    if (val == "infinity") return DAV_DEPTH_INFINITE;
    return val.empty() ? PVState::ABSENT : PVState::INVALID;
  }
  inline PValue<int8_t> _GetHeaderDepth(void) { return _GetHeaderSmallNum(DAV_HEADER_DEPTH); }

  PValue<FS::path> _GetHeaderDestination(void);

  esp_err_t _M_RELOC(bool duplicate);
  esp_err_t _DELETE(void);
  esp_err_t _M_SEND(bool content);
  esp_err_t _MKCOL(void);
  esp_err_t _OPTIONS(void);
  esp_err_t _PROPFIND(void);
  esp_err_t _PROPPATCH(void);
  esp_err_t _PUT(void);

  httpd_req_t* req_;
  const FS::path fs_path_;
  std::string host_prefix_;
};

esp_err_t DAVHandler::Run(void) {
#define HANDLE_METHOD(method) \
  case HTTP_##method:         \
    return _##method()
#define HANDLE_METHOD_M(method, handler, ...) \
  case HTTP_##method:                         \
    return _M_##handler(__VA_ARGS__)

  switch (req_->method) {
    HANDLE_METHOD_M(COPY, RELOC, true);
    HANDLE_METHOD_M(MOVE, RELOC, false);
    HANDLE_METHOD(DELETE);
    HANDLE_METHOD_M(GET, SEND, true);
    HANDLE_METHOD_M(HEAD, SEND, false);
    HANDLE_METHOD(MKCOL);
    HANDLE_METHOD(OPTIONS);
    HANDLE_METHOD(PROPFIND);
    HANDLE_METHOD(PROPPATCH);
    HANDLE_METHOD(PUT);
    default:
      return httpd_resp_send_err(req_, HTTPD_405_METHOD_NOT_ALLOWED, "Unsupported method");
  }
#undef HANDLE_METHOD
#undef HANDLE_METHOD_M

  // Should not reach
  // return ESP_OK;
}

inline esp_err_t _DAV_RELOC_FILE(const FS::path& src, const FS::path& dest,
                                 const PValue<bool>& overwrite, bool duplicate, httpd_req_t* req) {
  bool dest_exists = FS::exists(dest);
  if (dest_exists) {
    if (!overwrite.value_or(true)) {
      return httpd_resp_send_custom_err(req, DAV_STATUS_412_PRECONDITION_FAILED,
                                        "Destination file exists");
    }
  } else {
    FS::path dest_parent = dest.parent_path();
    if (!FS::is_directory(dest_parent)) {
      return httpd_resp_send_custom_err(req, DAV_STATUS_409_CONFLICT,
                                        "Destination parent dir DNE");
    }
  }

  // Perform file copy or move
  std::error_code ec;
  if (duplicate) {
    copy_options copt =
        overwrite.value_or(true) ? copy_options::overwrite_existing : copy_options::none;
    FS::copy_file(src, dest, copt, ec);
  } else {
    FS::rename(src, dest, ec);
  }
  // Process results
  if (ec.value() != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
  }

  if (dest_exists) {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  } else {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, DAV_STATUS_201_CREATED));
  }
  return httpd_resp_send(req, NULL, 0);
}

inline esp_err_t _DAV_RELOC_DIR(const FS::path& src, const FS::path& dest,
                                const PValue<bool>& overwrite, const PValue<int8_t>& depth,
                                bool duplicate, httpd_req_t* req) {
  bool trans_closure = depth.value_or(DAV_DEPTH_INFINITE) == DAV_DEPTH_INFINITE;
  if ((duplicate && !trans_closure && *depth != 0) || (!duplicate && !trans_closure)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported depth on dir");
  }

  bool dest_exists = FS::exists(dest);
  if (dest_exists) {
    if (!overwrite.value_or(true)) {
      return httpd_resp_send_custom_err(req, DAV_STATUS_412_PRECONDITION_FAILED,
                                        "Destination dir exists");
    }
    if (trans_closure && !FS::is_empty(dest)) {
      std::error_code ec;
      if (FS::remove_all(dest, ec), ec.value() != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
      }
    }
  } else {
    FS::path dest_parent = dest.parent_path();
    if (!FS::is_directory(dest_parent)) {
      return httpd_resp_send_custom_err(req, DAV_STATUS_409_CONFLICT,
                                        "Destination parent dir DNE");
    }
  }

  if (!trans_closure) {
    // Just need to create the destination directory
    // This can only happen when duplicate is true.
    if (dest_exists) {
      return httpd_resp_set_status(req, HTTPD_204);
    }
    std::error_code ec;
    if (FS::create_directory(dest, ec), ec.value() != 0) {
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
    }

    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, DAV_STATUS_201_CREATED));
    return httpd_resp_send(req, NULL, 0);
  }

  // Perform dir copy or move
  std::error_code ec;
  if (duplicate) {
    FS::copy(src, dest, copy_options::recursive, ec);
  } else {
    FS::rename(src, dest, ec);
  }
  // Process results
  if (ec.value() != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
  }

#ifndef DAV_COMPAT_PUT_ALWAYS_201
  if (dest_exists) {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  } else
#endif
  {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, DAV_STATUS_201_CREATED));
  }
  return httpd_resp_send(req, NULL, 0);
}

inline esp_err_t _DAV_SEND_FILE(const FS::path& src, httpd_req_t* req, bool content) {
  utils::AutoReleaseRes<FILE*> file(fopen(src.c_str(), "r"), [](FILE* file) {
    if (file) fclose(file);
  });
  if (*file == NULL) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to open file");
  }

  struct stat st = {};
  if (fstat(fileno(*file), &st) != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to get file stat");
  }

  utils::DataBufStash vcache;
  ESP_RETURN_ON_ERROR(_COMMON_FS_HEADERS(st, req, vcache));
  ESP_RETURN_ON_ERROR(vcache.AllocAndPrep(10, [&](utils::DataBuf& buf) {
    return httpd_resp_set_hdr(req, DAV_HEADER_CONTENT_LENGTH, buf.PrintTo("%ld", st.st_size));
  }));

  if (!content) {
    return httpd_resp_send(req, NULL, 0);
  }

  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTP_MIME_BINARY));
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, DAV_HEADER_CACHE_CONTROL, DAV_CACHE_CONTROL_VALUE));
  ESP_LOGD(TAG, "Sending %ld bytes...", st.st_size);
  size_t size = st.st_size;
  char buf[1024];
  while (size_t read_len = fread(buf, 1, sizeof(buf), *file)) {
    ZW_BREAK_ON_ERROR_SIMPLE(httpd_resp_send_chunk(req, buf, read_len));
    size -= read_len;
  }
  if (size) ESP_LOGW(TAG, "File read short by %d bytes", size);
  return httpd_resp_send_chunk(req, NULL, 0);
}

inline constexpr char _DAV_HTML_RESP_HEADER_STYLE[] =
    "<style>a{text-decoration:none;cursor:pointer;}a:hover{background:powderblue}</style>";
inline constexpr char _DAV_DIR_HTML_RESP_BODY_PREAMBLE[] = "<body><pre>";
inline constexpr char _DAV_DIR_HTML_RESP_BODY_POSTAMBLE[] = "</pre></body></html>";

#define DAV_DIR_HEADER_TMPL "<b>%-24.24s %8s  %-24s</b>\n"
#define DAV_DIR_LIST_TMPL "<a href=\"./%s\">%-24.24s %8s  %-24s</a>\n"
#define DAV_PRINT_DATE_TMPL "%F %T %Z"

inline const char* _DAV_DIR_ITEM(utils::DataBuf& buf, const char* src, const struct stat& st) {
  char size_buf[10];
  if (S_ISREG(st.st_mode)) {
    snprintf(size_buf, 10, "%ld", st.st_size);
  } else if (S_ISDIR(st.st_mode)) {
    strlcpy(size_buf, "(Folder)", 10);
  } else {
    ESP_LOGD(TAG, "Ignoring unsupported data '%s'", src);
    return nullptr;
  }

  char time_buf[25];
  struct tm lt;
  strftime(time_buf, 25, DAV_PRINT_DATE_TMPL, gmtime_r(&st.st_mtime, &lt));

  return buf.PrintTo(DAV_DIR_LIST_TMPL, src, src, size_buf, time_buf);
}

inline esp_err_t _DAV_SEND_DIR(const FS::path& src, httpd_req_t* req, bool content) {
  // Ensure the request Uri has a terminating slash
  if (req->uri[strlen(req->uri) - 1] != URI_PATH_DELIM) {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, DAV_STATUS_302_FOUND));
    std::string newloc(req->uri);
    newloc.push_back(URI_PATH_DELIM);
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, DAV_HEADER_LOCATION, newloc.c_str()));
    return httpd_resp_send(req, NULL, 0);
  }
  struct stat st = {};
  if (stat(src.c_str(), &st) != 0) {
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to get directory stat"));
  }
  utils::DataBufStash vcache;
  ESP_RETURN_ON_ERROR(_COMMON_FS_HEADERS(st, req, vcache));

  if (!content) {
    return httpd_resp_send(req, NULL, 0);
  }

  utils::DataBuf& fmt_buf = vcache.Allocate(65);
  ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, HTTP_MIME_HTML));
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, DAV_HEADER_CACHE_CONTROL, DAV_CACHE_CONTROL_VALUE));

  utils::DataBuf title_buf(32);
  title_buf.PrintTo("Directory contents of '%s'", src.c_str());
  ESP_RETURN_ON_ERROR(
      httpd_resp_send_chunk(req,
                            fmt_buf.PrintTo(DAV_HTML_RESP_HEADER_TMPL, (char*)title_buf.data(),
                                            _DAV_HTML_RESP_HEADER_STYLE),
                            HTTPD_RESP_USE_STRLEN));

  ESP_RETURN_ON_ERROR(
      httpd_resp_send_chunk(req, _DAV_DIR_HTML_RESP_BODY_PREAMBLE, HTTPD_RESP_USE_STRLEN));

  const char* entry_str = fmt_buf.PrintTo(DAV_DIR_HEADER_TMPL, "Name", "Size", "Last Modified");
  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, entry_str, HTTPD_RESP_USE_STRLEN));
  memset((char*)&fmt_buf.front(), '-', strlen(entry_str) - 1);
  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, entry_str, HTTPD_RESP_USE_STRLEN));

  FS::directory_iterator iter(src);
  for (; iter; ++iter) {
    const FS::path& entry = iter->path();
    st = {};
    if (stat(entry.c_str(), &st) != 0) {
      ESP_LOGW(TAG, "Unable to stat %s", entry.c_str());
      continue;
    }
    const char* entry_buf = _DAV_DIR_ITEM(fmt_buf, entry.filename().c_str(), st);
    if (entry_buf == nullptr) continue;
    ZW_BREAK_ON_ERROR_SIMPLE(httpd_resp_send_chunk(req, entry_buf, HTTPD_RESP_USE_STRLEN));
  }
  ESP_RETURN_ON_ERROR(
      httpd_resp_send_chunk(req, _DAV_DIR_HTML_RESP_BODY_POSTAMBLE, HTTPD_RESP_USE_STRLEN));
  return httpd_resp_send_chunk(req, NULL, 0);
}

inline esp_err_t _DAV_DELETE_FILE(const FS::path& src, httpd_req_t* req) {
  std::error_code ec;
  if (FS::remove(src, ec), ec.value() != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
  }
  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  return httpd_resp_send(req, NULL, 0);
}

inline esp_err_t _DAV_DELETE_DIR(const FS::path& src, PValue<int8_t> depth, httpd_req_t* req) {
  if (depth.value_or(DAV_DEPTH_INFINITE) != DAV_DEPTH_INFINITE) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid depth for delete dir");
  }
  std::error_code ec;
  if (FS::remove_all(src, ec), ec.value() != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
  }
  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  return httpd_resp_send(req, NULL, 0);
}

inline esp_err_t _DAV_CREATE_DIR(const FS::path& src, httpd_req_t* req) {
  FS::path dest_parent = src.parent_path();
  if (!FS::is_directory(dest_parent)) {
    return httpd_resp_send_custom_err(req, DAV_STATUS_409_CONFLICT, "Target parent dir DNE");
  }

  std::error_code ec;
  if (FS::create_directory(src, ec), ec.value() != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ec.message().c_str());
  }
  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, DAV_STATUS_201_CREATED));
  return httpd_resp_send(req, NULL, 0);
}

esp_err_t _DAV_RECV_FILE(const FS::path& dest, httpd_req_t* req) {
  bool dest_exists = FS::exists(dest);
  if (dest_exists && !FS::is_regular_file(dest)) {
    return httpd_resp_send_custom_err(req, DAV_STATUS_409_CONFLICT, "Target already exists");
  }
  size_t len_to_read = req->content_len;
  {
    utils::AutoReleaseRes<FILE*> file(fopen(dest.c_str(), "w"), [](FILE* file) {
      if (file) fclose(file);
    });
    if (*file == NULL) {
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to open file");
    }
    char buf[1024];
    while (len_to_read) {
      int read_len = std::min(len_to_read, (size_t)sizeof(buf));
      ZW_BREAK_ON_ERROR((read_len = httpd_req_recv(req, buf, read_len)) > 0 ? ESP_OK : ESP_FAIL, {},
                        ESP_LOGW(TAG, "Content receive error: %d", read_len));
      ZW_BREAK_ON_ERROR(fwrite(buf, 1, read_len, *file) == read_len ? ESP_OK : ESP_FAIL, {},
                        ESP_LOGW(TAG, "File write error"));
      len_to_read -= read_len;
    }
  }
  if (len_to_read) {
    ESP_LOGW(TAG, "Receive short by %d bytes", len_to_read);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not all data received");
  }
  if (dest_exists) {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_204));
  } else {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, DAV_STATUS_201_CREATED));
  }
  return httpd_resp_send(req, NULL, 0);
}

inline constexpr char _DAV_XML_RESP_FILE_PROP_TMPL[] =
    "<response><href>%s</href><propstat><prop>"
    "<getcontenttype>%s</getcontenttype>"
    "<getcontentlength>%ld</getcontentlength>"
    "<getetag>%s</getetag><getlastmodified>%s</getlastmodified>"
    "</prop><status>HTTP/1.1 200 OK</status>"
    "</propstat></response>";
inline constexpr char _DAV_XML_RESP_COLL_PROP_TMPL[] =
    "<response><href>%s</href><propstat><prop>"
    "<resourcetype><collection/></resourcetype>"
    "<getetag>%s</getetag><getlastmodified>%s</getlastmodified>"
    "</prop><status>HTTP/1.1 200 OK</status>"
    "</propstat></response>";

inline const char* _DAV_ITEM_PROP(utils::DataBuf& buf, const char* src, const struct stat& st) {
  utils::DataBuf href_buf(48);
  href_buf.PrintTo(_URI_PATTERN_ROOT "%s", src);

  char time_buf[32];
  struct tm lt;
  strftime(time_buf, 32, HTTP_DATE_TMPL, gmtime_r(&st.st_mtime, &lt));

  utils::DataBuf etag_buf(16);
  etag_buf.PrintTo("%06lX:%08lX", st.st_size & 0xffffff, st.st_mtime);

  if (S_ISREG(st.st_mode)) {
    return buf.PrintTo(_DAV_XML_RESP_FILE_PROP_TMPL, (char*)href_buf.data(), HTTP_MIME_BINARY,
                       st.st_size, (char*)etag_buf.data(), time_buf);
  } else if (S_ISDIR(st.st_mode)) {
    return buf.PrintTo(_DAV_XML_RESP_COLL_PROP_TMPL, (char*)href_buf.data(), (char*)etag_buf.data(),
                       time_buf);
  }

  ESP_LOGW(TAG, "Ignoring unsupported data '%s'", src);
  return nullptr;
}

esp_err_t _DAV_PROPFIND(const FS::path& src, uint8_t depth, httpd_req_t* req) {
  struct stat st = {};
  if (stat(src.c_str(), &st) != 0) {
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to stat"));
  }
  do {
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, HTTPD_207));
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, DAV_XML_RESP_TYPE));
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, DAV_XML_RESP_PREAMBLE, HTTPD_RESP_USE_STRLEN));
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, DAV_MULTISTAT_PREAMBLE, HTTPD_RESP_USE_STRLEN));

    utils::DataBuf buf;
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_chunk(req, _DAV_ITEM_PROP(buf, src.c_str(), st), HTTPD_RESP_USE_STRLEN));

    if (depth == 0 || !S_ISDIR(st.st_mode)) break;

    FS::directory_iterator iter(src);
    for (; iter; ++iter) {
      const FS::path& entry = iter->path();
      st = {};
      if (stat(entry.c_str(), &st) != 0) {
        ESP_LOGW(TAG, "Unable to stat %s", entry.c_str());
        continue;
      }
      const char* entry_buf = _DAV_ITEM_PROP(buf, entry.c_str(), st);
      if (entry_buf == nullptr) continue;
      ZW_BREAK_ON_ERROR_SIMPLE(httpd_resp_send_chunk(req, entry_buf, HTTPD_RESP_USE_STRLEN));
    }
  } while (0);

  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, DAV_MULTISTAT_POSTAMBLE, HTTPD_RESP_USE_STRLEN));
  return httpd_resp_send_chunk(req, NULL, 0);
}

PValue<FS::path> DAVHandler::_GetHeaderDestination(void) {
  std::string dest_uri = _GetHeader(DAV_HEADER_DESTINATION);
  if (dest_uri.empty()) return PVState::ABSENT;

  if (dest_uri.compare(0, host_prefix_.length(), host_prefix_) != 0 ||
      dest_uri.data()[host_prefix_.length()] != URI_PATH_DELIM) {
    ESP_LOGW(TAG, "! Unsupported remote destination");
    return PVState::INVALID;
  }
  const char* root_path = dest_uri.data() + host_prefix_.length();
  const char* rel_path = root_path + utils::STRLEN(URI_PATTERN_ROOT);
  if (strncmp(root_path, URI_PATTERN_ROOT, utils::STRLEN(URI_PATTERN_ROOT)) != 0 ||
      (*rel_path != '\0' && *rel_path != URI_PATH_DELIM)) {
    ESP_LOGW(TAG, "! Destination path out of range");
    return PVState::INVALID;
  }

  FS::path result = _NORMALIZE_PATH(rel_path);
  ESP_LOGD(TAG, "Destination Path = %s", result.c_str());

  if (result.is_relative()) {
    ESP_LOGW(TAG, "! Destination out-of-range");
    return PVState::INVALID;
  }
  return result;
}

esp_err_t DAVHandler::_M_RELOC(bool duplicate) {
  if (!FS::exists(fs_path_)) {
    return httpd_resp_send_err(req_, HTTPD_404_NOT_FOUND, "Source does not exist");
  }
  auto overwrite = _GetHeaderOverwrite();
  auto depth = _GetHeaderDepth();
  if (overwrite.state() == PVState::INVALID || depth.state() == PVState::INVALID) {
    return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Invalid header data");
  }

  PValue<FS::path> fs_dest_path = _GetHeaderDestination();
  if (fs_dest_path.state() == PVState::ABSENT) {
    return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Missing destination header");
  }
  if (fs_dest_path.state() == PVState::INVALID) {
    return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Invalid destination header");
  }
  if (*fs_dest_path == fs_path_) {
    return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Destination same as source");
  }

  if (FS::is_regular_file(fs_path_)) {
    if (depth.value_or(0) != 0) {
      return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Unsupported depth on file");
    }
    return _DAV_RELOC_FILE(fs_path_, *fs_dest_path, overwrite, duplicate, req_);
  } else if (FS::is_directory(fs_path_)) {
    return _DAV_RELOC_DIR(fs_path_, *fs_dest_path, overwrite, depth, duplicate, req_);
  }
  return httpd_resp_send_err(req_, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Unsupported source");
}

esp_err_t DAVHandler::_DELETE(void) {
  if (!FS::exists(fs_path_)) {
    return httpd_resp_send_err(req_, HTTPD_404_NOT_FOUND, "Source does not exist");
  }
  auto depth = _GetHeaderDepth();
  if (depth.state() == PVState::INVALID) {
    return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Invalid depth header");
  }
  if (FS::is_regular_file(fs_path_)) {
    if (depth.value_or(0) != 0) {
      return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Unsupported delete depth on file");
    }
    return _DAV_DELETE_FILE(fs_path_, req_);
  }
  if (FS::is_directory(fs_path_)) {
    return _DAV_DELETE_DIR(fs_path_, depth, req_);
  }
  return httpd_resp_send_err(req_, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Unsupported source");
}

esp_err_t DAVHandler::_M_SEND(bool content) {
  if (!FS::exists(fs_path_)) {
    return httpd_resp_send_err(req_, HTTPD_404_NOT_FOUND, "Source does not exist");
  }
  if (FS::is_regular_file(fs_path_))
    return _DAV_SEND_FILE(fs_path_, req_, content);
  else if (FS::is_directory(fs_path_))
    return _DAV_SEND_DIR(fs_path_, req_, content);
  return httpd_resp_send_err(req_, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Unsupported source");
}

esp_err_t DAVHandler::_MKCOL(void) {
  if (FS::exists(fs_path_)) {
    return httpd_resp_send_custom_err(req_, DAV_STATUS_409_CONFLICT, "Target already exists");
  }
  if (req_->content_len != 0) {
    return httpd_resp_send_custom_err(req_, DAV_STATUS_415_UNSUPPORTED_MEDIA_TYPE,
                                      "Content not accepted");
  }
  return _DAV_CREATE_DIR(fs_path_, req_);
}

esp_err_t DAVHandler::_OPTIONS(void) {
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req_, DAV_HEADER_DAV, "1"));
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req_, DAV_HEADER_ACCEPT_RANGES, "none"));
  std::string method_list;
  method_list.reserve(64);
  for (auto m : {HTTP_COPY, HTTP_DELETE, HTTP_GET, HTTP_HEAD, HTTP_MKCOL, HTTP_MOVE, HTTP_OPTIONS,
                 HTTP_PROPFIND, HTTP_PROPPATCH, HTTP_PUT}) {
    method_list.append(http_method_str(m)).push_back(',');
  }
  ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req_, DAV_HEADER_ALLOW, method_list.c_str()));
  ESP_RETURN_ON_ERROR(httpd_resp_set_status(req_, HTTPD_204));
  return httpd_resp_send(req_, NULL, 0);
}

esp_err_t DAVHandler::_PROPFIND(void) {
  if (!FS::exists(fs_path_)) {
    return httpd_resp_send_err(req_, HTTPD_404_NOT_FOUND, "Source does not exist");
  }
  auto depth = _GetHeaderDepth();
  if (!depth) {
    ESP_LOGW(TAG, "Depth = %d (%d)", *depth, static_cast<int>(depth.state()));
    return httpd_resp_send_err(req_, HTTPD_400_BAD_REQUEST, "Missing or invalid depth header");
  }
  if (*depth == DAV_DEPTH_INFINITE) {
    utils::DataBuf buf(100);
    std::string href = host_prefix_ + req_->uri;
    return httpd_resp_send_err(req_, HTTPD_403_FORBIDDEN,
                               _DAV_XML_ERROR(buf, DAV_TAG_INFINITE_DEPTH, href.c_str()));
  }
  // We ignore the request body (which may specify property filters), because
  // we only have very few attributes and properly parsing xml is too much hassle.
  return _DAV_PROPFIND(fs_path_, *depth, req_);
}

esp_err_t DAVHandler::_PROPPATCH(void) {
  if (!FS::exists(fs_path_)) {
    return httpd_resp_send_err(req_, HTTPD_404_NOT_FOUND, "Source does not exist");
  }
  // The only meaningful attribute to support is `getlastmodified`
  // (or `lastmodified` -- there is even confusing on this).
  //
  // IMO that is too much trouble for such a tiny device.
  // But per http://www.webdav.org/specs/rfc2518.html#METHOD_PROPPATCH,
  // support for PROPPATCH is required, so we just handle it as a
  // general access rejection.
  return httpd_resp_send_err(req_, HTTPD_403_FORBIDDEN, "Disallowed by file system");
}

esp_err_t DAVHandler::_PUT(void) {
#ifdef __ESP_FS_HAS_VFSSTAT
  auto space_info = FS::space(fs_path_);
  if (space_info.available < req_->content_len + SPI_FLASH_SEC_SIZE) {
    return httpd_resp_send_custom_err(req_, DAV_STATUS_409_CONFLICT, "Insufficient space");
  }
#endif
  return _DAV_RECV_FILE(fs_path_, req_);
}

esp_err_t _handler_webdav(httpd_req_t* req) {
  auto handler = DAVHandler::Create(req);
  if (handler) handler->Run();
  ESP_LOGD(TAG, "+> Heap: %d; Stack: %d", esp_get_free_heap_size(),
           uxTaskGetStackHighWaterMark(NULL));
  return ESP_OK;
}

}  // namespace

esp_err_t register_handler_webdav(httpd_handle_t httpd) {
  ESP_LOGD(TAG, "Register handler on %s", URI_PATTERN);
  httpd_uri_t handler = {
      .uri = URI_PATTERN,
      .method = HTTP_ANY,
      .handler = _handler_webdav,
      .user_ctx = NULL,
  };
  return httpd_register_uri_handler(httpd, &handler);
}

}  // namespace zw::esp8266::app::httpd

#endif  // ZW_APPLIANCE_COMPONENT_WEBDAV