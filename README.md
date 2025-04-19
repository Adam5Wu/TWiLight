# ESP ZWAppliance - Take 2

A generic framework and foundation for *rapid* IoT appliances creation.

This is a complete re-take of my [earlier project](https://github.com/Adam5Wu/ESPZWAppliance).

## Feature Comparison

Below is a comparison table to highlight the feature differences.

| Feature                                  | Old               | New                                  |
| ---------------------------------------- | ----------------- | ------------------------------------ |
| Development Environment                  | Arduino           | **VSCode + PlatformIO**              |
| Lower-level framework                    | ESP8266-Arduino   | **(Enhanced) ESP8266 RTOS SDK**      |
| &#8195;&#8600; Concurrent multi-tasking  | No                | **Yes**                              |
| Embedded File system                     | FATFS             | **LittleFS**                         |
| &#8195;&#8600; System data               | In PROGMEM        | **On System Partition**              |
| Appliance Configuration                  | JSON File         | JSON File                            |
| &#8195;&#8600; Data access               | Self-managed      | **Framework API**                    |
| &#8195;&#8600; Backup and restore        | Self-managed      | **File system image + Web UI**       |
| &#8195;&#8600; Factory reset             | Self-managed      | **Automatic with Layered FS**        |
| RTC memory access                        | Yes               | Yes                                  |
| &#8195;&#8600; Management API            | No                | **Yes**                              |
| Time management                          | Yes               | Yes                                  |
| &#8195;&#8600; RTC tracking & recovery   | Yes               | Yes                                  |
| &#8195;&#8600; NTP time sync             | Yes               | Yes                                  |
| &#8195;&#8600; Smooth time adjustment    | Probably No?      | **Yes**                              |
| Network Provisioning                     | Yes               | Yes                                  |
| &#8195;&#8600; With Web page             | Backend Only      | **Backend + Frontend**               |
| &#8195;&#8600; With Espressif App        | No                | **Yes**                              |
| &#8195;&#8600; With WPS                  | Yes               | **No** (Maybe &#9733;)               |
| &#8195;&#8600; Auto engage / disengage   | Yes               | Yes                                  |
| &#8195;&#8600; Captive Portal            | Yes               | Yes                                  |
| Web server                               | Yes               | Yes                                  |
| &#8195;&#8600; Authentication            | Yes               | **No** (Maybe &#9733;&#9733;)        |
| &#8195;&#8600; Caching & ETag validation | Yes               | Yes                                  |
| &#8195;&#8600; Interactive development   | Custom PUT/DELETE | **WebDAV**                           |
| FTP server                               | Yes               | **No** (Maybe &#9733;)               |
| OTA firmware update                      | Backend Only      | **Backend + Frontend**               |
| &#8195;&#8600; Dual images & toggling    | Self-managed      | **Backend + Frontend**               |

* For `No` features in the new framework, having a `Maybe` tag means they maybe added later,
and the number of &#9733;s denotes their *relative* priority.
* The official maintenance of the ESP8266 RTOS SDK seem to have paused, I maintain an
[enhanced fork](https://github.com/Adam5Wu/ESP8266_RTOS_SDK/tree/deploy/live),
with multiple bug fixes, backports and feature improvements.

## How to Create Appliance

1. Install VSCode and PlatformIO extension, if you don't have them already;
2. From the GitHub UI, click "Use this template" to create a new repo;
3. Checkout locally abd open the root folder with PlatformIO;
4. Off you go! (No, really! You are ready!)

If you need a pointer of where to start:
- Update `ZWAppConfig.h`:
  - Customize `_ZW_APPLIANCE_NAME`, which also controls the SoftAP prefix;
  - Adjust `ZW_APPLIANCE_COMPONENT_*` to configure features.
- Create one or more sub-directories to host your appliance logic:
  - Implement an `init()` function to perform basic initialization.
  - Usually, subscribe to `ZW_SYSTEM_EVENT_NET_STA_IP_READY` to start service logic.
    - Avoid performing long operations (e.g. service loop) in this event handler.
    - Instead, create a task to run them, and return from the event handler ASAP.
  - If your service logic desires to start running *before* connecting to the network:
    - Start a task running your service loop in `init()`;
    - Observe the network status by checking `ZW_SYSTEM_STATE_NET_STA_IP_READY`.
  - General advice / best practices:
    - Put all code and declarations in your own namespace;
    - Refer to other `App*` modules for code patterns / examples.
- Install your modules(s) into `main.cpp`:
  - Place their `init()` calls before the `// Initialization is now complete...`;
