#include "net_diag.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "router_globals.h"

#define DIAG_TASK_STACK_SIZE 4096
#define DIAG_TASK_PRIORITY 4
#define DIAG_PROBE_TIMEOUT_MS 2000
#define DIAG_PERIODIC_PROBE_MS 15000

static const char *TAG = "NET_DIAG";
static TaskHandle_t s_diag_task_handle;
static bool s_napt_enabled;
static esp_err_t s_last_napt_err = ESP_OK;
static bool s_portal_authenticated;
static char s_pending_reason[48] = "boot";

static void format_ip4(uint32_t addr, char *out, size_t out_len)
{
    esp_ip4_addr_t ip = { .addr = addr };
    esp_ip4addr_ntoa(&ip, out, (int)out_len);
}

static void log_netif_state(const char *label, esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_dns_info_t dns_info;
    char ip_str[16] = "n/a";
    char gw_str[16] = "n/a";
    char mask_str[16] = "n/a";
    char dns_str[16] = "n/a";

    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        esp_ip4addr_ntoa(&ip_info.gw, gw_str, sizeof(gw_str));
        esp_ip4addr_ntoa(&ip_info.netmask, mask_str, sizeof(mask_str));
    }

    if (netif != NULL &&
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
        esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
    }

    ESP_LOGI(TAG, "[%s] ip=%s gw=%s mask=%s dns=%s", label, ip_str, gw_str, mask_str, dns_str);
}

void net_diag_log_snapshot(const char *reason)
{
    char ap_ip_str[16] = "n/a";
    char sta_ip_str[16] = "n/a";

    format_ip4(my_ap_ip, ap_ip_str, sizeof(ap_ip_str));
    format_ip4(my_ip, sta_ip_str, sizeof(sta_ip_str));

    ESP_LOGI(TAG,
             "[SNAPSHOT:%s] uplink=%d portal=%d napt=%d napt_err=%s stations=%u ap_ip=%s sta_ip=%s",
             reason != NULL ? reason : "unknown",
             ap_connect,
             s_portal_authenticated,
             s_napt_enabled,
             esp_err_to_name(s_last_napt_err),
             connect_count,
             ap_ip_str,
             sta_ip_str);
    log_netif_state("AP", wifiAP);
    log_netif_state("STA", wifiSTA);
}

static bool tcp_probe_ipv4(uint32_t addr, uint16_t port, const char *label)
{
    struct sockaddr_in remote_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = addr,
    };
    struct sockaddr_in local_bind_addr = { 0 };
    struct timeval timeout = {
        .tv_sec = DIAG_PROBE_TIMEOUT_MS / 1000,
        .tv_usec = (DIAG_PROBE_TIMEOUT_MS % 1000) * 1000,
    };
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    esp_ip4_addr_t ip = { .addr = addr };

    if (sock < 0) {
        ESP_LOGW(TAG, "[PROBE:%s] socket create failed errno=%d", label, errno);
        return false;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (my_ip != 0) {
        local_bind_addr.sin_family = AF_INET;
        local_bind_addr.sin_port = htons(0);
        local_bind_addr.sin_addr.s_addr = my_ip;
        if (bind(sock, (const struct sockaddr *)&local_bind_addr, sizeof(local_bind_addr)) < 0) {
            ESP_LOGW(TAG, "[PROBE:%s] bind to STA IP failed errno=%d", label, errno);
        }
    }

    if (connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) == 0) {
        ESP_LOGI(TAG, "[PROBE:%s] tcp://" IPSTR ":%u ok", label, IP2STR(&ip), port);
        close(sock);
        return true;
    }

    ESP_LOGW(TAG, "[PROBE:%s] tcp://" IPSTR ":%u failed errno=%d", label, IP2STR(&ip), port, errno);
    close(sock);
    return false;
}

static void run_active_probes(const char *reason)
{
    esp_netif_ip_info_t sta_info;
    esp_netif_dns_info_t dns_info;

    ESP_LOGI(TAG, "[PROBE:%s] starting active network probes", reason != NULL ? reason : "unknown");
    net_diag_log_snapshot(reason != NULL ? reason : "probe");

    if (wifiSTA == NULL || esp_netif_get_ip_info(wifiSTA, &sta_info) != ESP_OK) {
        ESP_LOGW(TAG, "[PROBE:%s] STA netif not ready; skipping probes", reason != NULL ? reason : "unknown");
        return;
    }

    if (sta_info.gw.addr != 0) {
        tcp_probe_ipv4(sta_info.gw.addr, 53, "gateway:53");
    }

    if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4 &&
        dns_info.ip.u_addr.ip4.addr != 0) {
        tcp_probe_ipv4(dns_info.ip.u_addr.ip4.addr, 53, "sta-dns:53");
    }

    tcp_probe_ipv4(inet_addr("8.8.8.8"), 53, "google-dns:53");
    tcp_probe_ipv4(inet_addr("1.1.1.1"), 80, "cloudflare:80");
}

static void net_diag_task(void *arg)
{
    int64_t last_probe_time = 0;

    (void)arg;

    net_diag_log_snapshot("diag_task_start");

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        if (s_pending_reason[0] != '\0') {
            run_active_probes(s_pending_reason);
            s_pending_reason[0] = '\0';
            last_probe_time = esp_timer_get_time();
            continue;
        }

        if (s_portal_authenticated && ap_connect) {
            int64_t now = esp_timer_get_time();
            if ((now - last_probe_time) >= (DIAG_PERIODIC_PROBE_MS * 1000LL)) {
                run_active_probes("periodic");
                last_probe_time = now;
            }
        }
    }
}

void net_diag_start_task(void)
{
    if (s_diag_task_handle != NULL) {
        return;
    }

    xTaskCreate(net_diag_task, "net_diag", DIAG_TASK_STACK_SIZE, NULL, DIAG_TASK_PRIORITY, &s_diag_task_handle);
}

void net_diag_schedule_probe(const char *reason)
{
    if (reason != NULL) {
        strlcpy(s_pending_reason, reason, sizeof(s_pending_reason));
    } else {
        strlcpy(s_pending_reason, "manual", sizeof(s_pending_reason));
    }

    if (s_diag_task_handle != NULL) {
        xTaskNotifyGive(s_diag_task_handle);
    }
}

void net_diag_set_napt_state(bool enabled, esp_err_t last_err)
{
    s_napt_enabled = enabled;
    s_last_napt_err = last_err;
    net_diag_log_snapshot(enabled ? "napt_enabled" : "napt_disabled");
}

void net_diag_set_portal_state(bool authenticated, const char *reason)
{
    s_portal_authenticated = authenticated;
    net_diag_log_snapshot(reason != NULL ? reason : (authenticated ? "portal_on" : "portal_off"));
}
