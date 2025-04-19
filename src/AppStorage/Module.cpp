#include "Module.hpp"

#include <string>
#include <vector>
#include <memory>
#include <utility>

#include <stdint.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>

#include "FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "rtc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_spi_flash.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"

#include "FreeRTOS.h"
#include "freertos/task.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface.hpp"

#define RTC_MEM_STRONG_INTEGRITY 0

#if RTC_MEM_STRONG_INTEGRITY
#include "rom/md5_hash.h"
#define RTC_MEM_DIGEST_LEN 16
#else
#include "esp_crc.h"
#define RTC_MEM_DIGEST_LEN 4
#endif

#define RTC_MEM_USER_SIZE (RTC_USER_SIZE - RTC_MEM_DIGEST_LEN)
#define RTC_MEM_DIGEST_ADDR (RTC_USER_BASE + RTC_MEM_USER_SIZE)

namespace zw::esp8266::app::storage {
namespace {

inline constexpr char TAG[] = "Storage";

//-------------------
// NVS support
//-------------------

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

esp_err_t _nvs_init(void) {
  ESP_RETURN_ON_ERROR(nvs_flash_init());
  nvs_stats_t stats;
  ESP_RETURN_ON_ERROR(nvs_get_stats(NULL, &stats));
  size_t pmr = (stats.free_entries * 10000ULL + stats.total_entries / 2) / stats.total_entries;
  ESP_LOGI(TAG, "- Provisioned %d entries", stats.total_entries);
  ESP_LOGI(TAG, "- Contains %d namespaces and %d entries (%d.%02d%% free)", stats.namespace_count,
           stats.used_entries, pmr / 100, pmr % 100);
#ifndef NDEBUG
  _nvs_log();
#endif
  return ESP_OK;
}

//-------------------
// RTCMEM support
//-------------------

xSemaphoreHandle rtcmem_lock_;

void _rtcmem_digest(void *holder) {
#if RTC_MEM_STRONG_INTEGRITY
  MD5Context md5_context;
  MD5Init(&md5_context);
  MD5Update(&md5_context, (const unsigned char *)RTC_USER_BASE, RTC_MEM_USER_SIZE);
  MD5Final((unsigned char *)holder, &md5_context);
#else
  *(uint32_t *)holder = crc32_le(0, (const uint8_t *)RTC_USER_BASE, RTC_MEM_USER_SIZE);
#endif
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, holder, RTC_MEM_DIGEST_LEN, ESP_LOG_DEBUG);
}

bool _rtcmem_integrity_check() {
  char digest[RTC_MEM_DIGEST_LEN];
  _rtcmem_digest(digest);
  return memcmp(digest, (void *)RTC_MEM_DIGEST_ADDR, RTC_MEM_DIGEST_LEN) == 0;
}

esp_err_t _rtcmem_init(void) {
  ESP_LOGI(TAG, "- Total %d bytes, %d allocable", RTC_USER_SIZE, RTC_MEM_USER_SIZE);
  if (_rtcmem_integrity_check()) {
    ESP_LOGI(TAG, "- Integrity check passed!");
    ESP_LOGD(TAG, "Dump of RTC memory (%d bytes):", RTC_USER_SIZE);
    ESP_LOG_BUFFER_HEXDUMP(TAG, (void *)RTC_USER_BASE, RTC_USER_SIZE, ESP_LOG_DEBUG);
  } else {
    ESP_LOGI(TAG, "- Digest mismatch, re-initializing...");
    memset((void *)RTC_USER_BASE, 0, RTC_MEM_USER_SIZE);
    _rtcmem_digest((void *)RTC_MEM_DIGEST_ADDR);
  }

  ESP_RETURN_ON_ERROR(
      (rtcmem_lock_ = xSemaphoreCreateMutex(), rtcmem_lock_ == NULL ? ESP_FAIL : ESP_OK));

  return ESP_OK;
}

size_t rtc_alloc_top_ = RTC_USER_BASE;

//-------------------
// Flash FS support
//-------------------

inline void _fs_log(const char *mount, size_t total, size_t free) {
  // ESP_LOGI(TAG, "%-10s %4fKB (%.2f%% free)", mount, total / 1024, free * 100.0 / total);
  size_t pmr = (free * 10000ULL + total / 2) / total;
  ESP_LOGI(TAG, "%-10s %4dKB (%d.%02d%% free)", mount, total / 1024, pmr / 100, pmr % 100);
}

esp_err_t _fs_mount(const char *mount, const char *part_label, bool auto_format, bool read_only,
                    bool log_info = true) {
  esp_vfs_littlefs_conf_t conf = {
      .base_path = mount,
      .partition_label = part_label,
      .partition = NULL,
      .format_if_mount_failed = auto_format,
      .read_only = read_only,
      .dont_mount = false,
      .grow_on_mount = true,
  };
  ESP_RETURN_ON_ERROR(esp_vfs_littlefs_register(&conf));
  if (log_info) {
    size_t total = 0, used = 0;
    ESP_RETURN_ON_ERROR(esp_littlefs_info(part_label, &total, &used));
    _fs_log(mount, total, total - used);
  }
  return ESP_OK;
}

class DataPartitionXAImpl : public DataPartitionXA, public utils::AutoRelease {
 public:
  DataPartitionXAImpl(utils::AutoRelease &&vfs_lock_releaser, const esp_partition_t *part)
      : utils::AutoRelease(std::move(vfs_lock_releaser)), part_(part) {
    assert(part != nullptr && (part->address % SPI_FLASH_SEC_SIZE == 0) &&
           (part->size % SPI_FLASH_SEC_SIZE == 0));
  }

  size_t sectors(void) const override { return part_->size / SPI_FLASH_SEC_SIZE; }

  esp_err_t read_sector(size_t sector, void *buf) const override {
    return esp_partition_read(part_, sector * SPI_FLASH_SEC_SIZE, buf, SPI_FLASH_SEC_SIZE);
  }

  esp_err_t write_sector(size_t sector, const void *buf) const override {
    ESP_RETURN_ON_ERROR(
        esp_partition_erase_range(part_, sector * SPI_FLASH_SEC_SIZE, SPI_FLASH_SEC_SIZE));
    return esp_partition_write(part_, sector * SPI_FLASH_SEC_SIZE, buf, SPI_FLASH_SEC_SIZE);
  }

 protected:
  const esp_partition_t *part_;
};

}  // namespace

esp_err_t RTCAlloc::update(const void *in, size_t size) {
  // Must not roll over the end
  if ((uint32_t)base_ + size > RTC_MEM_DIGEST_ADDR) return ESP_ERR_INVALID_SIZE;
  // We will wait forever, don't expect failure
  xSemaphoreTake(rtcmem_lock_, portMAX_DELAY);
  memcpy(base_, in, size);
  // Must re-compute the digest
  _rtcmem_digest((void *)RTC_MEM_DIGEST_ADDR);
  xSemaphoreGive(rtcmem_lock_);
  return ESP_OK;
}

esp_err_t RTCRawData::update(const void *in, size_t size) {
  // Must not go beyond the allocated size
  if (size > size_) return ESP_ERR_INVALID_SIZE;
  return RTCAlloc::update(in, size);
}

RTCRawData rtcmem_alloc(size_t size) {
  // Access is 4-byte aligned, so we round up
  size = (size + 3) / 4;
  if (rtc_alloc_top_ + size > RTC_MEM_DIGEST_ADDR) {
    return RTCRawData(nullptr, 0);
  }
  RTCRawData result((void *)rtc_alloc_top_, size);
  rtc_alloc_top_ += size;
  return result;
}

esp_err_t init(void) {
  ESP_LOGI(TAG, "Checking RTC memory...");
  ESP_RETURN_ON_ERROR(_rtcmem_init());

  ESP_LOGI(TAG, "Initializing NVS...");
  ESP_RETURN_ON_ERROR(_nvs_init());

  ESP_LOGI(TAG, "Mounting system partition...");
  ESP_RETURN_ON_ERROR(_fs_mount(ZW_SYSTEM_MOUNT_POINT, ZW_SYSTEM_PART_LABEL, false, true));

  ESP_LOGI(TAG, "Mounting storage partition...");
  ESP_RETURN_ON_ERROR(_fs_mount(ZW_STORAGE_MOUNT_POINT, ZW_STORAGE_PART_LABEL, true, false));
  // Enable VFS locking to support backup and restore.
  ESP_RETURN_ON_ERROR(esp_vfs_lock_enable(ZW_STORAGE_MOUNT_POINT));

  return ESP_OK;
}

esp_err_t remount_system_rw(void) {
  ESP_LOGI(TAG, "Re-mounting system partition read/write...");
  ESP_RETURN_ON_ERROR(esp_vfs_littlefs_unregister(ZW_SYSTEM_PART_LABEL));
  ESP_RETURN_ON_ERROR(_fs_mount(ZW_SYSTEM_MOUNT_POINT, ZW_SYSTEM_PART_LABEL, false, false, false));
  return ESP_OK;
}

void finit(void) {
  // Take the RTC memory lock, to avoid incomplete updates.
  xSemaphoreTake(rtcmem_lock_, portMAX_DELAY);
}

utils::DataOrError<std::unique_ptr<DataPartitionXA>> data_partition_access(bool for_write) {
  const esp_partition_t *data_part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, ZW_STORAGE_PART_LABEL);
  if (data_part == NULL) return ESP_ERR_NOT_FOUND;

  ESP_RETURN_ON_ERROR(esp_vfs_lock_acquire(ZW_STORAGE_MOUNT_POINT));
  utils::AutoRelease vfs_lock_releaser([] { esp_vfs_lock_release(ZW_STORAGE_MOUNT_POINT); });
  if (for_write) {
    size_t open_fd_count;
    ESP_RETURN_ON_ERROR(esp_vfs_open_fd_count(ZW_STORAGE_MOUNT_POINT, &open_fd_count));
    if (open_fd_count > 0) return ESP_ERR_INVALID_STATE;
  }
  // Give a generous amount for time for any carry-over activities to clear up.
  vTaskDelay(CONFIG_FREERTOS_HZ / 10);
  return {std::make_unique<DataPartitionXAImpl>(std::move(vfs_lock_releaser), data_part)};
}

}  // namespace zw::esp8266::app::storage