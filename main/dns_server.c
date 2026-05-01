#include "dns_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "router_globals.h"
#include "net_diag.h"

#define DNS_PORT 53
#define DNS_BUFFER_SIZE 512
#define DNS_TASK_STACK_SIZE 4096
#define DNS_TASK_PRIORITY 5
#define DNS_TIMEOUT_MS 2000
#define DNS_TTL_SECONDS 60
#define DEFAULT_UPSTREAM_DNS "8.8.8.8"
#define SECONDARY_UPSTREAM_DNS "1.1.1.1"

static const char *TAG = "DNS_SERVER";
static TaskHandle_t s_dns_task_handle;

static size_t dns_skip_name(const uint8_t *packet, size_t packet_len, size_t offset)
{
    while (offset < packet_len) {
        uint8_t len = packet[offset++];

        if (len == 0) {
            return offset;
        }

        if ((len & 0xC0) == 0xC0) {
            return (offset < packet_len) ? offset + 1 : 0;
        }

        if ((offset + len) > packet_len) {
            return 0;
        }

        offset += len;
    }

    return 0;
}

static bool dns_is_a_record_query(const uint8_t *packet, size_t packet_len)
{
    size_t question_end = dns_skip_name(packet, packet_len, 12);
    uint16_t qtype;
    uint16_t qclass;

    if (question_end == 0 || (question_end + 4) > packet_len) {
        return false;
    }

    memcpy(&qtype, packet + question_end, sizeof(qtype));
    memcpy(&qclass, packet + question_end + 2, sizeof(qclass));

    return ntohs(qtype) == 1 && ntohs(qclass) == 1;
}

static bool dns_get_question_info(const uint8_t *packet,
                                  size_t packet_len,
                                  char *name_out,
                                  size_t name_out_len,
                                  uint16_t *qtype_out)
{
    size_t offset = 12;
    size_t name_pos = 0;
    size_t question_end;
    uint16_t qtype;

    if (packet_len < 16 || name_out == NULL || name_out_len == 0 || qtype_out == NULL) {
        return false;
    }

    name_out[0] = '\0';

    while (offset < packet_len) {
        uint8_t label_len = packet[offset++];

        if (label_len == 0) {
            break;
        }

        if ((label_len & 0xC0) != 0 || (offset + label_len) > packet_len) {
            return false;
        }

        if (name_pos > 0 && name_pos < (name_out_len - 1)) {
            name_out[name_pos++] = '.';
        }

        for (uint8_t i = 0; i < label_len && name_pos < (name_out_len - 1); i++) {
            name_out[name_pos++] = (char)packet[offset + i];
        }
        name_out[name_pos] = '\0';
        offset += label_len;
    }

    question_end = offset;
    if (question_end == 0 || (question_end + 4) > packet_len) {
        return false;
    }

    memcpy(&qtype, packet + question_end, sizeof(qtype));
    *qtype_out = ntohs(qtype);
    return true;
}

static ssize_t build_dns_response(const uint8_t *query, size_t query_len, uint8_t *response, size_t response_size, bool answer_with_portal_ip)
{
    size_t question_end;

    if (query_len < 12 || response_size < query_len || response_size < 32) {
        return -1;
    }

    question_end = dns_skip_name(query, query_len, 12);
    if (question_end == 0 || (question_end + 4) > query_len) {
        return -1;
    }

    memcpy(response, query, question_end + 4);

    response[2] = 0x81;
    response[3] = 0x80;
    response[6] = 0x00;
    response[7] = answer_with_portal_ip ? 0x01 : 0x00;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;

    if (!answer_with_portal_ip) {
        return (ssize_t)(question_end + 4);
    }

    if ((question_end + 20) > response_size) {
        return -1;
    }

    response[question_end + 4] = 0xC0;
    response[question_end + 5] = 0x0C;
    response[question_end + 6] = 0x00;
    response[question_end + 7] = 0x01;
    response[question_end + 8] = 0x00;
    response[question_end + 9] = 0x01;
    response[question_end + 10] = 0x00;
    response[question_end + 11] = 0x00;
    response[question_end + 12] = 0x00;
    response[question_end + 13] = DNS_TTL_SECONDS;
    response[question_end + 14] = 0x00;
    response[question_end + 15] = 0x04;
    memcpy(response + question_end + 16, &my_ap_ip, sizeof(my_ap_ip));

    return (ssize_t)(question_end + 20);
}

static ssize_t build_dns_servfail_response(const uint8_t *query, size_t query_len, uint8_t *response, size_t response_size)
{
    ssize_t response_len = build_dns_response(query, query_len, response, response_size, false);

    if (response_len > 0) {
        response[3] = 0x82;
    }

    return response_len;
}

static bool get_upstream_dns_addr(struct sockaddr_in *upstream_addr)
{
    esp_netif_dns_info_t dns_info;

    memset(upstream_addr, 0, sizeof(*upstream_addr));
    upstream_addr->sin_family = AF_INET;
    upstream_addr->sin_port = htons(DNS_PORT);

    if (wifiSTA != NULL &&
        esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4 &&
        dns_info.ip.u_addr.ip4.addr != 0) {
        upstream_addr->sin_addr.s_addr = dns_info.ip.u_addr.ip4.addr;
        return true;
    }

    return inet_pton(AF_INET, DEFAULT_UPSTREAM_DNS, &upstream_addr->sin_addr) == 1;
}

static bool fill_dns_addr(const char *ip_str, struct sockaddr_in *upstream_addr)
{
    memset(upstream_addr, 0, sizeof(*upstream_addr));
    upstream_addr->sin_family = AF_INET;
    upstream_addr->sin_port = htons(DNS_PORT);
    return inet_pton(AF_INET, ip_str, &upstream_addr->sin_addr) == 1;
}

static bool forward_dns_query_to_server(const struct sockaddr_in *upstream_addr, const uint8_t *query, size_t query_len, uint8_t *response, ssize_t *response_len)
{
    struct sockaddr_in local_bind_addr = { 0 };
    struct sockaddr_in local_addr = { 0 };
    socklen_t local_addr_len = sizeof(local_addr);
    struct timeval timeout = {
        .tv_sec = DNS_TIMEOUT_MS / 1000,
        .tv_usec = (DNS_TIMEOUT_MS % 1000) * 1000,
    };
    int upstream_sock;

    upstream_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (upstream_sock < 0) {
        ESP_LOGW(TAG, "Failed to create upstream DNS socket: errno=%d", errno);
        return false;
    }

    setsockopt(upstream_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(upstream_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (my_ip != 0) {
        local_bind_addr.sin_family = AF_INET;
        local_bind_addr.sin_port = htons(0);
        local_bind_addr.sin_addr.s_addr = my_ip;

        if (bind(upstream_sock, (const struct sockaddr *)&local_bind_addr, sizeof(local_bind_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind upstream DNS socket to STA IP " IPSTR ": errno=%d",
                     IP2STR((const ip4_addr_t *)&my_ip), errno);
        }
    }

    if (getsockname(upstream_sock, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
        ESP_LOGI(TAG, "Upstream DNS socket local endpoint " IPSTR ":%u",
                 IP2STR((const ip4_addr_t *)&local_addr.sin_addr.s_addr), ntohs(local_addr.sin_port));
    }

    if (sendto(upstream_sock, query, query_len, 0, (const struct sockaddr *)upstream_addr, sizeof(*upstream_addr)) < 0) {
        ESP_LOGW(TAG, "Failed to send DNS query upstream to " IPSTR ": errno=%d",
                 IP2STR((const ip4_addr_t *)&upstream_addr->sin_addr.s_addr), errno);
        close(upstream_sock);
        return false;
    }

    *response_len = recvfrom(upstream_sock, response, DNS_BUFFER_SIZE, 0, NULL, NULL);
    close(upstream_sock);

    if (*response_len <= 0) {
        ESP_LOGW(TAG, "Timed out waiting for DNS response from " IPSTR,
                 IP2STR((const ip4_addr_t *)&upstream_addr->sin_addr.s_addr));
        return false;
    }

    ESP_LOGI(TAG, "Received %d-byte DNS response from " IPSTR,
             (int)*response_len,
             IP2STR((const ip4_addr_t *)&upstream_addr->sin_addr.s_addr));

    return *response_len > 0;
}

static bool forward_dns_query(const uint8_t *query, size_t query_len, uint8_t *response, ssize_t *response_len)
{
    struct sockaddr_in upstream_addr;
    struct sockaddr_in fallback_addr;

    if (get_upstream_dns_addr(&upstream_addr)) {
        ESP_LOGI(TAG, "Forwarding DNS query to upstream " IPSTR,
                 IP2STR((const ip4_addr_t *)&upstream_addr.sin_addr.s_addr));
        if (forward_dns_query_to_server(&upstream_addr, query, query_len, response, response_len)) {
            return true;
        }
    }

    if (fill_dns_addr(DEFAULT_UPSTREAM_DNS, &fallback_addr) &&
        fallback_addr.sin_addr.s_addr != upstream_addr.sin_addr.s_addr) {
        ESP_LOGI(TAG, "Retrying DNS query via fallback %s", DEFAULT_UPSTREAM_DNS);
        if (forward_dns_query_to_server(&fallback_addr, query, query_len, response, response_len)) {
            return true;
        }
    }

    if (fill_dns_addr(SECONDARY_UPSTREAM_DNS, &fallback_addr) &&
        fallback_addr.sin_addr.s_addr != upstream_addr.sin_addr.s_addr) {
        ESP_LOGI(TAG, "Retrying DNS query via fallback %s", SECONDARY_UPSTREAM_DNS);
        if (forward_dns_query_to_server(&fallback_addr, query, query_len, response, response_len)) {
            return true;
        }
    }

    return false;
}

static void dns_server_task(void *arg)
{
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int sock;
    int reuse_addr = 1;
    uint8_t query[DNS_BUFFER_SIZE];
    uint8_t response[DNS_BUFFER_SIZE];

    (void)arg;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on port %d", DNS_PORT);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        ssize_t query_len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        ssize_t response_len = -1;
        bool authenticated;
        char qname[96] = "<unknown>";
        uint16_t qtype = 0;

        if (query_len <= 0) {
            continue;
        }

        if (query_len < 12 || (query[2] & 0x80) != 0) {
            continue;
        }

        authenticated = is_client_session_active(client_addr.sin_addr.s_addr);
        dns_get_question_info(query, (size_t)query_len, qname, sizeof(qname), &qtype);

        if (authenticated) {
            ESP_LOGI(TAG, "Authenticated DNS query from " IPSTR " qtype=%u name=%s",
                     IP2STR((ip4_addr_t *)&client_addr.sin_addr.s_addr),
                     qtype,
                     qname);
            if (!forward_dns_query(query, (size_t)query_len, response, &response_len)) {
                ESP_LOGW(TAG, "All upstream DNS attempts failed for client " IPSTR,
                         IP2STR((ip4_addr_t *)&client_addr.sin_addr.s_addr));
                net_diag_log_snapshot("dns_forward_failed");
                net_diag_schedule_probe("dns_forward_failed");
                response_len = build_dns_servfail_response(query, (size_t)query_len, response, sizeof(response));
            }
        } else {
            ESP_LOGI(TAG, "Captive DNS response for unauthenticated client " IPSTR " qtype=%u name=%s",
                     IP2STR((ip4_addr_t *)&client_addr.sin_addr.s_addr),
                     qtype,
                     qname);
            response_len = build_dns_response(
                query,
                (size_t)query_len,
                response,
                sizeof(response),
                dns_is_a_record_query(query, (size_t)query_len));
        }

        if (response_len > 0) {
            sendto(sock, response, (size_t)response_len, 0, (struct sockaddr *)&client_addr, client_addr_len);
        }
    }
}

void start_dns_server(void)
{
    if (s_dns_task_handle != NULL) {
        return;
    }

    xTaskCreate(dns_server_task, "dns_server", DNS_TASK_STACK_SIZE, NULL, DNS_TASK_PRIORITY, &s_dns_task_handle);
}
