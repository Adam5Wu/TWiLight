#ifndef ZWAPP_CONFIG
#define ZWAPP_CONFIG

#include <sdkconfig.h>

#define _ZW_APPLIANCE_NAME "ZWBase"
inline constexpr char ZW_APPLIANCE_NAME[] = _ZW_APPLIANCE_NAME;
inline constexpr char ZW_APPLIANCE_AP_PREFIX[] = _ZW_APPLIANCE_NAME "-";

#define _ZW_SYSTEM_PART_LABEL "system"
inline constexpr char ZW_SYSTEM_PART_LABEL[] = _ZW_SYSTEM_PART_LABEL;
inline constexpr char ZW_SYSTEM_MOUNT_POINT[] = "/" _ZW_SYSTEM_PART_LABEL;

#define _ZW_STORAGE_PART_LABEL "storage"
inline constexpr char ZW_STORAGE_PART_LABEL[] = _ZW_STORAGE_PART_LABEL;
inline constexpr char ZW_STORAGE_MOUNT_POINT[] = "/" _ZW_STORAGE_PART_LABEL;

#ifdef CONFIG_ESP8266_TIME_SYSCALL_USE_FRC1
#define ZW_SYSTIME_AVAILABLE
#endif

// Stagger cold boot to avoid in-rush power consumption leading to
// some under-buffered programming adapter to fail USB negotiation.
#ifdef CONFIG_RESET_REASON
#define CONFIG_COLD_BOOT_STAGGER_MS 1000
#endif

#ifdef CONFIG_BOOTLOADER_FAST_BOOT
// Enable use of fastboot (skips factory reset check on "hot" boot)
#define ZW_APPLIANCE_FASTBOOT
#endif

// Enable Captive DNS during network provisioning (+0.6KB app)
#define ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS
// Enable WebDAV for development (!! +156KB app)
#define ZW_APPLIANCE_COMPONENT_WEBDAV

// Enable Web-based system management function (+2.5KB app)
#define ZW_APPLIANCE_COMPONENT_WEB_SYSFUNC
#ifdef ZW_APPLIANCE_COMPONENT_WEB_SYSFUNC

// Enable Web-based network provisioning (+0.6KB app)
#define ZW_APPLIANCE_COMPONENT_WEB_NET_PROVISION
// Enable Web-OTA support (+1.5KB app)
#define ZW_APPLIANCE_COMPONENT_WEB_OTA

#endif

// Enable recursive lock for config access
// ... so that a task won't block itself performing overlapped locked accesses.
// For example, calling `persist()` while holding a live `XAppConfig` object.
#define ZW_APPLIANCE_COMPONENT_CONFIG_RECURSIVE_LOCK

#ifdef ZW_SYSTIME_AVAILABLE

// Enable tracking time in RTC (so that time after reboot is more accurate)
#define ZW_APPLIANCE_COMPONENT_TIME_RTC_TRACKING
// Enable time sync with NTP
#define ZW_APPLIANCE_COMPONENT_TIME_SNTP
// Define an alternative limit for smooth time adjustment
#define ZW_APPLIANCE_COMPONENT_TIME_SMOOTH_LIMIT 600

#endif  // ZW_SYSTIME_AVAILABLE

#endif  // ZWAPP_CONFIG