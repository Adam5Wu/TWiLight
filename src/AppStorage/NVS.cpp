#include <string>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface.hpp"
#include "Interface_Private.hpp"

namespace zw::esp8266::app::storage {
namespace {

inline constexpr char TAG[] = "Storage:NVS";

#ifndef NDEBUG

#define NVS_ENTRY_NUM_I "> %s::%s (%s) = %d (%x)"
#define NVS_ENTRY_NUM_U "> %s::%s (%s) = %u (%x)"
#define NVS_ENTRY_NUM_64 "> %s::%s (%s) = 0x%x%08x"
#define NVS_ENTRY_STR "> %s::%s (STR) = '%s'"
#define NVS_ENTRY_BLOB "> %s::%s (BLOB:%d):"
#define NVS_ENTRY_UNKNOWN "> %s::%s: "

static std::string nvs_ns_;
static nvs_handle_t nvs_handle_;

esp_err_t _nvs_log_entry(nvs_entry_info_t entry) {
  if (nvs_ns_ != entry.namespace_name) {
    if (!nvs_ns_.empty()) {
      ESP_LOGD(TAG, "Closing NS: %s", nvs_ns_.c_str());
      nvs_close(nvs_handle_);
    }
    ESP_LOGD(TAG, "Opening NS: %s", entry.namespace_name);
    ESP_RETURN_ON_ERROR(nvs_open(entry.namespace_name, NVS_READONLY, &nvs_handle_));
    nvs_ns_ = entry.namespace_name;
  }

#define CASE_FETCH_NUM(e, tpfx, ftpfx, cpfx, width)                                             \
  case NVS_TYPE_##cpfx##width: {                                                                \
    tpfx##width##_t val;                                                                        \
    ESP_RETURN_ON_ERROR(nvs_get_##ftpfx##width(nvs_handle_, e.key, &val));                      \
    ESP_LOGI(TAG, NVS_ENTRY_NUM_##cpfx, e.namespace_name, e.key, #cpfx #width, (tpfx##32_t)val, \
             (tpfx##32_t)val);                                                                  \
  } break;

#define CASE_FETCH_NUM64(e, tpfx, ftpfx, cpfx)                                                  \
  case NVS_TYPE_##cpfx##64: {                                                                   \
    tpfx##64_t val;                                                                             \
    ESP_RETURN_ON_ERROR(nvs_get_##ftpfx##64(nvs_handle_, e.key, &val));                         \
    ESP_LOGI(TAG, NVS_ENTRY_NUM_64, e.namespace_name, e.key, #cpfx "64", (uint32_t)(val >> 32), \
             (uint32_t)val);                                                                    \
  } break;

  switch (entry.type) {
    CASE_FETCH_NUM(entry, uint, u, U, 8);
    CASE_FETCH_NUM(entry, uint, u, U, 16);
    CASE_FETCH_NUM(entry, uint, u, U, 32);
    CASE_FETCH_NUM(entry, int, i, I, 8);
    CASE_FETCH_NUM(entry, int, i, I, 16);
    CASE_FETCH_NUM(entry, int, i, I, 32);
    CASE_FETCH_NUM64(entry, uint, u, U);
    CASE_FETCH_NUM64(entry, int, i, I);

    case NVS_TYPE_STR: {
      size_t len = 0;
      ESP_RETURN_ON_ERROR(nvs_get_str(nvs_handle_, entry.key, NULL, &len));
      std::vector<char> val(len);
      ESP_RETURN_ON_ERROR(nvs_get_str(nvs_handle_, entry.key, &val.front(), &len));
      ESP_LOGI(TAG, NVS_ENTRY_STR, entry.namespace_name, entry.key, &val.front());
    } break;

    case NVS_TYPE_BLOB: {
      size_t len = 0;
      ESP_RETURN_ON_ERROR(nvs_get_blob(nvs_handle_, entry.key, NULL, &len));
      std::vector<char> val(len);
      ESP_RETURN_ON_ERROR(nvs_get_blob(nvs_handle_, entry.key, &val.front(), &len));
      ESP_LOGI(TAG, NVS_ENTRY_BLOB, entry.namespace_name, entry.key, len);
      ESP_LOG_BUFFER_HEXDUMP(TAG, val.data(), len, ESP_LOG_DEBUG);
    } break;

    default:
      ESP_LOGI(TAG, NVS_ENTRY_UNKNOWN "Unknown data type %d", entry.namespace_name, entry.key,
               entry.type);
  }
  return ESP_OK;
}

void _nvs_log(void) {
  ESP_LOGI(TAG, "NVS Entries:");
  nvs_iterator_t iter = nvs_entry_find(NULL, NULL, NVS_TYPE_ANY);
  while (iter) {
    nvs_entry_info_t entry;
    nvs_entry_info(iter, &entry);
    _nvs_log_entry(entry);
    iter = nvs_entry_next(iter);
  }
  if (!nvs_ns_.empty()) nvs_close(nvs_handle_);
}
#endif

}  // namespace

esp_err_t nvs_init(void) {
  ESP_RETURN_ON_ERROR(nvs_flash_init());
  nvs_stats_t stats;
  ESP_RETURN_ON_ERROR(nvs_get_stats(NULL, &stats));
  size_t pmr = (stats.free_entries * 10000ULL + stats.total_entries / 2) / stats.total_entries;
  ESP_LOGD(TAG, "Provisioned %d entries", stats.total_entries);
  ESP_LOGI(TAG, "Contains %d namespaces and %d entries (%d.%02d%% free)", stats.namespace_count,
           stats.used_entries, pmr / 100, pmr % 100);
#ifndef NDEBUG
  _nvs_log();
#endif
  return ESP_OK;
}

void nvs_finit(void) {
  // Nothing to do
}

}  // namespace zw::esp8266::app::storage