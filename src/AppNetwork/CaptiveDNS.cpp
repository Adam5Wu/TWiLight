#include "CaptiveDNS.hpp"

#include <string>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"

#include "lwip/sockets.h"
#include "lwip/err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Interface_Private.hpp"

#include "AppConfig/Interface.hpp"
#include "AppEventMgr/Interface.hpp"

#include "ZWUtils.hpp"
#include "ZWAppConfig.h"

#ifdef ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS

#define CAPTIVE_DNS_MESSAGE_LIMIT 512

namespace zw::esp8266::app::network {
namespace {

inline constexpr char TAG[] = "CapDNS";

using ::zw::esp8266::app::config::AppConfig;

typedef struct __attribute__((packed)) {
  uint16_t ID;
  union {
    uint8_t Flags;
    struct __attribute__((packed)) {
      unsigned char RD : 1;      // recursion desired
      unsigned char TC : 1;      // truncated message
      unsigned char AA : 1;      // authoritative answer
      unsigned char OPCode : 4;  // message_type
      unsigned char QR : 1;      // query/response flag
    };
  };
  union {
    uint8_t RFlags;
    struct __attribute__((packed)) {
      unsigned char RCode : 4;  // response code
      unsigned char Z : 3;      // its z! reserved
      unsigned char RA : 1;     // recursion available
    };
  };
  uint16_t QDCount;
  uint16_t ANCount;
  uint16_t NSCount;
  uint16_t ARCount;
} DNSHeader;

typedef struct __attribute__((packed)) {
  uint16_t QType;
  uint16_t QClass;
} DNSQueryFooter;

typedef struct __attribute__((packed)) {
  uint8_t RefCode;
  uint8_t RefOffset;
  uint16_t Type;
  uint16_t Class;
  uint32_t TTL;
  uint16_t RQLen;
} DNSAnswerHeader;

#define DNS_QR_QUERY 0
#define DNS_QR_RESPONSE 1
#define DNS_OPCODE_QUERY 0

#define DNS_REPLY_NOERROR 0
#define DNS_REPLY_FAILURE 2

ip_addr_t ap_ipaddr_;

esp_err_t _handle_request(const struct sockaddr_in &src_addr, socklen_t src_len,
                          const utils::DataBuf &req, ssize_t req_len, int sock_fd) {
  if (req_len < sizeof(DNSHeader)) {
    ESP_LOGD(TAG, "Request too small (%d)", req_len);
    // Data may not even be valid, don't handle
    return ESP_OK;
  }

  // Parse the name being queried
  std::string hostname;
  auto header = (const DNSHeader *)req.data();
  ssize_t consumed_len = sizeof(DNSHeader);
  // const DNSQueryFooter *footer;
  {
    if (header->QR != DNS_QR_QUERY || header->OPCode != DNS_OPCODE_QUERY) {
      // Not interested, don't handle
      return ESP_OK;
    }
    if (header->QDCount == 0) {
      // We don't know how to handle this
      ESP_LOGD(TAG, "Request without hostname");
      goto resp_failed;
    }
    const char *token = (char *)req.data() + consumed_len;
    while (*token) {
      if (!hostname.empty()) hostname.push_back('.');
      uint8_t token_len = *token;
      if (consumed_len + token_len + 2 + sizeof(DNSQueryFooter) > req_len) {
        // Do not go over the boundary
        ESP_LOGD(TAG, "Request data overflow");
        goto resp_failed;
      }
      hostname.append(token + 1, token_len);
      token += token_len + 1;
      consumed_len += token_len + 1;
    }
    // footer = (const DNSQueryFooter *)(token + 1);
    consumed_len += sizeof(DNSQueryFooter) + 1;
    ESP_LOGD(TAG, "Received query for host '%s'", hostname.c_str());
  }
  ESP_LOG_BUFFER_HEXDUMP(TAG, req.data(), req_len, ESP_LOG_DEBUG);

  // Prepare common response data.
  {
    utils::DataBuf resp(consumed_len + sizeof(DNSAnswerHeader) + sizeof(ip_addr_t));
    auto resp_hdr = (DNSHeader *)&resp.front();
    *resp_hdr = *header;
    resp_hdr->QR = DNS_QR_RESPONSE;
    resp_hdr->RA = 0;
    resp_hdr->RCode = DNS_REPLY_NOERROR;
    resp_hdr->QDCount = htons(1);
    resp_hdr->ANCount = htons(1);
    resp_hdr->ARCount = 0;
    resp_hdr->NSCount = 0;
    memcpy(&resp.front() + sizeof(DNSHeader), req.data() + sizeof(DNSHeader),
           consumed_len - sizeof(DNSHeader));
    auto answer_hdr = (DNSAnswerHeader *)(&resp.front() + consumed_len);
    answer_hdr->RefCode = 0xc0;
    answer_hdr->RefOffset = 0x0c;
    answer_hdr->Type = htons(0x1);   // Type A
    answer_hdr->Class = htons(0x1);  // Class IN
    answer_hdr->TTL = htonl(60);
    answer_hdr->RQLen = htons(sizeof(ip_addr_t));
    auto answer_data = &resp.front() + consumed_len + sizeof(DNSAnswerHeader);
    memcpy(answer_data, &ap_ipaddr_, sizeof(ip_addr_t));
    ESP_LOGD(TAG, "Responding A '%s'", ipaddr_ntoa(&ap_ipaddr_));
    ESP_LOG_BUFFER_HEXDUMP(TAG, resp.data(), resp.size(), ESP_LOG_DEBUG);
    if (sendto(sock_fd, resp.data(), resp.size(), 0, (const sockaddr *)&src_addr, src_len) < 0) {
      ESP_LOGW(TAG, "Failed to send response");
    }
  }
  return ESP_OK;

resp_failed:
  DNSHeader resp = {};
  resp.ID = header->ID;
  resp.QR = DNS_QR_RESPONSE;
  resp.RCode = DNS_REPLY_FAILURE;
  ESP_LOGD(TAG, "Respond SERVFAIL");
  ESP_LOG_BUFFER_HEXDUMP(TAG, &resp, sizeof(DNSHeader), ESP_LOG_DEBUG);
  if (sendto(sock_fd, &resp, sizeof(DNSHeader), 0, (const sockaddr *)&src_addr, src_len) < 0) {
    ESP_LOGW(TAG, "Failed to send response");
  }
  return ESP_OK;
}

}  // namespace

// We assume this task is only run *after* entering provisioning mode.
// i.e. the SoftAP has been set up and running.
void captive_dns_task(void *) {
  utils::AutoReleaseRes<int> server_sock;
  ESP_LOGD(TAG, "Configuring AP DHCP...");
  {
    tcpip_adapter_ip_info_t ip_info;
    ESP_GOTO_ON_ERROR(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info), failed);
    ESP_LOGD(TAG, "AP IP address: %s", ipaddr_ntoa(&ip_info.ip));
    ap_ipaddr_ = ip_info.ip;
    tcpip_adapter_dns_info_t dns_addr{.ip = ip_info.ip};
    ESP_GOTO_ON_ERROR(
        tcpip_adapter_set_dns_info(TCPIP_ADAPTER_IF_AP, TCPIP_ADAPTER_DNS_MAIN, &dns_addr), failed);
  }

  ESP_LOGD(TAG, "Starting service...");
  {
    server_sock = utils::AutoReleaseRes<int>(socket(AF_INET, SOCK_DGRAM, 0), [](int sock) {
      if (sock != -1) close(sock);
    });
    if (*server_sock == -1) {
      ESP_LOGE(TAG, "Failed to create socket");
      eventmgr::SetSystemFailed();
      return;
    }

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(53);
    server_addr.sin_len = sizeof(server_addr);

    if (bind(*server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
      ESP_LOGE(TAG, "Failed to start listening");
      eventmgr::SetSystemFailed();
      return;
    }
  }

  ESP_LOGD(TAG, "Listening for DNS queries...");
  while (eventmgr::system_states_peek(ZW_SYSTEM_STATE_NET_STA_IP_READY) == 0) {
    struct sockaddr_in src_addr = {};
    socklen_t src_len = sizeof(struct sockaddr_in);

    utils::DataBuf req(CAPTIVE_DNS_MESSAGE_LIMIT);
    if (ssize_t req_len = recvfrom(*server_sock, &req.front(), CAPTIVE_DNS_MESSAGE_LIMIT, 0,
                                   (struct sockaddr *)&src_addr, (socklen_t *)&src_len);
        req_len > 0) {
      ESP_GOTO_ON_ERROR(_handle_request(src_addr, src_len, req, req_len, *server_sock), failed);
    }
  }
  ESP_LOGD(TAG, "Stopping service...");
  return;

failed:
  ESP_LOGE(TAG, "Error processing request");
  eventmgr::SetSystemFailed();
}

}  // namespace zw::esp8266::app::network

#endif  // ZW_APPLIANCE_COMPONENT_NET_CAPTIVE_DNS