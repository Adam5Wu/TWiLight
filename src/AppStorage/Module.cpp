#include "Module.hpp"

#include <string>
#include <vector>
#include <memory>
#include <utility>

#include <stdint.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"

#include "FreeRTOS.h"
#include "freertos/task.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface.hpp"
#include "Interface_Private.hpp"

namespace zw::esp8266::app::storage {
namespace {

inline constexpr char TAG[] = "Storage";

inline void _fs_log(const char *mount, size_t total, size_t free) {
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

esp_err_t init(void) {
  ESP_RETURN_ON_ERROR(rtcmem_init());
  ESP_RETURN_ON_ERROR(nvs_init());

  ESP_LOGD(TAG, "Mounting system partition...");
  ESP_RETURN_ON_ERROR(_fs_mount(ZW_SYSTEM_MOUNT_POINT, ZW_SYSTEM_PART_LABEL, false, true));

  ESP_LOGD(TAG, "Mounting storage partition...");
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
  esp_vfs_littlefs_unregister(ZW_STORAGE_PART_LABEL);
  esp_vfs_littlefs_unregister(ZW_SYSTEM_PART_LABEL);
  nvs_finit();
  rtcmem_finit();
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