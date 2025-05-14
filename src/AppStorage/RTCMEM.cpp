#include <string.h>

#include "FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "rtc.h"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#include "Interface.hpp"
#include "Interface_Private.hpp"

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

inline constexpr char TAG[] = "Storage:RTCMEM";

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

size_t rtc_alloc_top_ = RTC_USER_BASE;

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

esp_err_t rtcmem_init(void) {
  ESP_LOGI(TAG, "Total %d bytes, %d allocable", RTC_USER_SIZE, RTC_MEM_USER_SIZE);
  if (_rtcmem_integrity_check()) {
    ESP_LOGD(TAG, "Integrity check passed!");
    ESP_LOGD(TAG, "Dump of RTC memory (%d bytes):", RTC_USER_SIZE);
    ESP_LOG_BUFFER_HEXDUMP(TAG, (void *)RTC_USER_BASE, RTC_USER_SIZE, ESP_LOG_DEBUG);
  } else {
    ESP_LOGI(TAG, "Digest mismatch, re-initializing...");
    memset((void *)RTC_USER_BASE, 0, RTC_MEM_USER_SIZE);
    _rtcmem_digest((void *)RTC_MEM_DIGEST_ADDR);
  }

  ESP_RETURN_ON_ERROR(
      (rtcmem_lock_ = xSemaphoreCreateMutex(), rtcmem_lock_ == NULL ? ESP_FAIL : ESP_OK));

  return ESP_OK;
}

void rtcmem_finit(void) {
  // Take the RTC memory lock, to avoid incomplete updates.
  xSemaphoreTake(rtcmem_lock_, portMAX_DELAY);
}

}  // namespace zw::esp8266::app::storage