
#ifndef APPSTORAGE_INTERFACE
#define APPSTORAGE_INTERFACE

#include <memory>

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_system.h"

#include "ZWUtils.hpp"

namespace zw::esp8266::app::storage {

class RTCAlloc {
 public:
  RTCAlloc() = default;

  // Returns false if allocation was unsuccessful.
  operator bool() const { return base_; }

 protected:
  RTCAlloc(void* base) : base_(base) {}
  esp_err_t update(const void* in, size_t size);

  void* base_;
};

// Simple raw access to a piece of allocated RTC memory.
class RTCRawData : public RTCAlloc {
 public:
  const void* get(void) const { return base_; }
  esp_err_t update(const void* in, size_t size);

 protected:
  friend RTCRawData rtcmem_alloc(size_t size);
  RTCRawData(void* base, size_t size) : RTCAlloc(base), size_(size) {}

  size_t size_;
};

// Static typed access to a piece of allocated RTC memory.
template <typename T>
class RTCData : public RTCAlloc {
 public:
  RTCData() = default;

  const T* get(void) const { return (const T*)base_; }
  esp_err_t put(const T* in) { return update(in, sizeof(T)); }

  const T& operator*() const { return *get(); }
  const T* operator->() const { return get(); }

  RTCData& operator=(const T& in) {
    if (put(&in) != ESP_OK) ets_printf("RTCData assignment failed!");
    return *this;
  }

 protected:
  template <typename X>
  friend RTCData<X> rtcmem_alloc(void);

  // Since size is statically defined, no need to track it.
  RTCData(RTCRawData&& alloc) : RTCAlloc(alloc) {}
};

// Allocate an RTC memory region of given size.
// Note that the allocation does NOT initialize (e.g zero-fill)
// since the entire point of putting data in RTC memory is to
// persist across reboot.
//
// Warning: the order of allocation is *VERY IMPORTANT*!
// The most meaningful use is to call this function inside a
// deterministic part of initialization.
//
// Allocation can fail if the space ran out. The caller should
// check using the bool() operator before use.
extern RTCRawData rtcmem_alloc(size_t size);

// Allocate a (statically) typed RTC memory region.
template <typename T>
RTCData<T> rtcmem_alloc(void) {
  return RTCData<T>(rtcmem_alloc(sizeof(T)));
}

// Get an exclusive access to the data partition.
// Useful for backup and restore.
class DataPartitionXA {
 public:
  DataPartitionXA() = default;
  virtual ~DataPartitionXA() = default;

  DataPartitionXA(const DataPartitionXA&) = delete;
  DataPartitionXA& operator=(const DataPartitionXA&) = delete;
  DataPartitionXA(DataPartitionXA&&) = default;
  DataPartitionXA& operator=(DataPartitionXA&&) = default;

  // Number of flash sectors
  virtual size_t sectors(void) const = 0;
  // Read the content of a specific flash sector
  virtual esp_err_t read_sector(size_t sector, void* buf) const = 0;
  // Write the buffer to a specific flash sector
  virtual esp_err_t write_sector(size_t sector, const void* buf) const = 0;
};

// Acquire exclusive data partition for read or write.
// - If acquiring for read, there can be open file descriptors, this is because
//   the underlying file system (littleFS) is crash resilient, so a dump of the
//   partition will still be consistent.
// - If acquiring for write, any open file descriptor will result in failure.
utils::DataOrError<std::unique_ptr<DataPartitionXA>> data_partition_access(bool for_write);

}  // namespace zw::esp8266::app::storage

#endif  // APPSTORAGE_INTERFACE
