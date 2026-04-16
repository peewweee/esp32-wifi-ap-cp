/*
 * Captive Portal HTTP Server
 *
 * Flow:
 * 1. User Connects -> Portal Page (/)
 * 2. User Clicks Start -> Redirects to /confirm
 * 3. /confirm -> Enables Internet, Starts Timer, Displays Link to Root PWA
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_spiffs.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include "esp_netif.h"
#include <esp_http_server.h>
#include "lwip/sockets.h" 

// Your project's global variables
#include "router_globals.h"

#include "pages.h"
#include "net_diag.h"
#include "supabase_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/base64.h" // Required for decoding the password
#include "mbedtls/sha256.h"

static const char *TAG_WEB = "CAPTIVE_PORTAL";
static const char *TAG_ADMIN = "ADMIN_SERVER";

// --- ADMIN CREDENTIALS ---
#ifndef ADMIN_DEFAULT_USERNAME
#define ADMIN_DEFAULT_USERNAME "admin"
#endif

#ifndef ADMIN_DEFAULT_PASSWORD
#define ADMIN_DEFAULT_PASSWORD "admin123"
#endif

#define AUTH_REALM "Admin Configuration"

// --- SESSION CONFIGURATION ---
#define MAX_CLIENTS 20
#define DAILY_QUOTA_SEC 3600
#define SESSION_DURATION_SEC DAILY_QUOTA_SEC
#define SESSION_DURATION_US (SESSION_DURATION_SEC * 1000000LL)
#define SESSION_TOKEN_HEX_LEN 32
#define SESSION_TOKEN_LEN (SESSION_TOKEN_HEX_LEN + 1)
#define DEVICE_HASH_LEN 65
#define QUOTA_BLOB_KEY "quota_tab_v1"
#define QUOTA_DAY_HINT_KEY "quota_day_hint"
#define QUOTA_FLUSH_INTERVAL_US (60 * 1000000LL)
#define PWA_DASHBOARD_URL "https://spcs-v1.vercel.app/dashboard"
#define PWA_LINK_URL_FORMAT PWA_DASHBOARD_URL "/link?session_token=%s"

// --- PER-DEVICE SESSION STRUCTURE ---
typedef struct {
    uint32_t ip_addr;       // The client's IP address
    uint8_t mac[6];         // The client's MAC address
    char token[SESSION_TOKEN_LEN];
    char device_hash[DEVICE_HASH_LEN];
    int64_t end_time;       // When their session expires (internal ESP time)
    int64_t start_time;
    uint32_t granted_seconds;
    uint32_t accounted_seconds;
    uint32_t day_id;
    bool active;            // Is this slot in use?
} client_session_t;

typedef struct {
    bool valid;
    uint8_t mac[6];
    uint32_t day_id;
    uint32_t used_seconds;
} quota_record_t;

static client_session_t sessions[MAX_CLIENTS];
static quota_record_t s_quota_records[MAX_CLIENTS];
static bool s_napt_enabled;
static int64_t s_global_session_end_time;
static bool s_heartbeat_task_started;
static bool s_quota_dirty;
static uint32_t s_day_hint;
static int64_t s_last_quota_flush_us;

// --- FORWARD DECLARATIONS ---
int get_remaining_seconds(uint32_t ip);
void start_session(uint32_t ip);
uint32_t get_client_ip(httpd_req_t *req);

static esp_err_t send_redirect_to_portal(httpd_req_t *req);
static esp_err_t send_probe_success(httpd_req_t *req);
static esp_err_t ensure_napt_enabled(void);
static int get_global_remaining_seconds(void);
static client_session_t *start_or_resume_session(uint32_t ip, const uint8_t *mac, const char *token, const char *device_hash, int granted_seconds);
static bool get_client_mac_for_ip(uint32_t ip, uint8_t mac_out[6]);
static bool get_any_connected_client_mac(uint8_t mac_out[6]);
static void generate_session_token(char token_out[SESSION_TOKEN_LEN]);
static void generate_hash_from_mac(const uint8_t mac[6], char device_hash_out[DEVICE_HASH_LEN]);
static void generate_hash_from_ip(uint32_t ip, char device_hash_out[DEVICE_HASH_LEN]);
static void ensure_heartbeat_task_started(void);
static void supabase_heartbeat_task(void *pvParameters);
static void refresh_portal_auth_state(const char *reason);
static void maybe_disable_napt_if_idle(const char *reason);
static void revoke_session(int session_index, bool disconnected, const char *reason);
static int get_quota_remaining_for_mac(const uint8_t mac[6], uint32_t *day_id_out);
static void checkpoint_session_usage(client_session_t *session, int64_t now, bool force_flush);
static esp_err_t send_quota_page(httpd_req_t *req);
static void load_quota_state(void);

esp_timer_handle_t restart_timer;

static void restart_timer_callback(void* arg)
{
    ESP_LOGI(TAG_ADMIN, "Restarting now...");
    esp_restart();
}

esp_timer_create_args_t restart_timer_args = {
        .callback = &restart_timer_callback,
        .arg = (void*) 0,
        .name = "restart_timer"
};

// --- SESSION MANAGER FUNCTIONS ---

void init_sessions() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        memset(&sessions[i], 0, sizeof(sessions[i]));
    }
    load_quota_state();
}

static int find_session_index_by_ip(uint32_t ip)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && sessions[i].ip_addr == ip) {
            return i;
        }
    }
    return -1;
}

static int find_session_index_by_mac(const uint8_t mac[6])
{
    if (mac == NULL) {
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && memcmp(sessions[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_session_index_by_token(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && strcmp(sessions[i].token, token) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_session_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            return i;
        }
    }
    return -1;
}

static bool has_valid_system_time(time_t *out_now)
{
    time_t now = time(NULL);

    if (out_now == NULL) {
        return false;
    }

    if (now < 1700000000) {
        return false;
    }

    *out_now = now;
    return true;
}

static uint32_t get_current_day_id(bool *have_real_time)
{
    time_t now = 0;

    if (has_valid_system_time(&now)) {
        uint32_t day_id = (uint32_t)(now / 86400);
        s_day_hint = day_id;
        if (have_real_time != NULL) {
            *have_real_time = true;
        }
        return day_id;
    }

    if (have_real_time != NULL) {
        *have_real_time = false;
    }
    return s_day_hint != 0 ? s_day_hint : 1;
}

static int get_seconds_until_next_day(void)
{
    time_t now = 0;

    if (!has_valid_system_time(&now)) {
        return DAILY_QUOTA_SEC;
    }

    time_t next_day = ((now / 86400) + 1) * 86400;
    int seconds = (int)(next_day - now);
    return seconds > 0 ? seconds : 1;
}

static bool mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t empty_mac[6] = {0};

    return mac == NULL || memcmp(mac, empty_mac, sizeof(empty_mac)) == 0;
}

static void flush_quota_state(bool force_flush)
{
    nvs_handle_t nvs;
    esp_err_t err;
    int64_t now = esp_timer_get_time();

    if (!s_quota_dirty) {
        return;
    }

    if (!force_flush && (now - s_last_quota_flush_us) < QUOTA_FLUSH_INTERVAL_US) {
        return;
    }

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_WEB, "Failed to open NVS for quota flush: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs, QUOTA_BLOB_KEY, s_quota_records, sizeof(s_quota_records));
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, QUOTA_DAY_HINT_KEY, s_day_hint);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG_WEB, "Failed to persist quota state: %s", esp_err_to_name(err));
        return;
    }

    s_quota_dirty = false;
    s_last_quota_flush_us = now;
}

static void load_quota_state(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(s_quota_records);
    esp_err_t err;

    memset(s_quota_records, 0, sizeof(s_quota_records));
    s_quota_dirty = false;
    s_day_hint = 0;
    s_last_quota_flush_us = esp_timer_get_time();

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_WEB, "Failed to open NVS for quota state: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_get_blob(nvs, QUOTA_BLOB_KEY, s_quota_records, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG_WEB, "Failed to load quota table: %s", esp_err_to_name(err));
        memset(s_quota_records, 0, sizeof(s_quota_records));
    } else if (err == ESP_OK && len != sizeof(s_quota_records)) {
        ESP_LOGW(TAG_WEB, "Quota table size mismatch; resetting stored quota state");
        memset(s_quota_records, 0, sizeof(s_quota_records));
    }

    err = nvs_get_u32(nvs, QUOTA_DAY_HINT_KEY, &s_day_hint);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG_WEB, "Failed to load quota day hint: %s", esp_err_to_name(err));
        s_day_hint = 0;
    }

    nvs_close(nvs);
}

static quota_record_t *find_or_alloc_quota_record(const uint8_t mac[6], uint32_t day_id)
{
    int free_index = -1;

    if (mac_is_zero(mac)) {
        return NULL;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!s_quota_records[i].valid) {
            if (free_index < 0) {
                free_index = i;
            }
            continue;
        }

        if (memcmp(s_quota_records[i].mac, mac, 6) == 0) {
            if (s_quota_records[i].day_id != day_id) {
                s_quota_records[i].day_id = day_id;
                s_quota_records[i].used_seconds = 0;
                s_quota_dirty = true;
            }
            return &s_quota_records[i];
        }
    }

    if (free_index < 0) {
        free_index = 0;
    }

    memset(&s_quota_records[free_index], 0, sizeof(s_quota_records[free_index]));
    s_quota_records[free_index].valid = true;
    memcpy(s_quota_records[free_index].mac, mac, 6);
    s_quota_records[free_index].day_id = day_id;
    s_quota_records[free_index].used_seconds = 0;
    s_quota_dirty = true;
    return &s_quota_records[free_index];
}

static int get_quota_remaining_for_mac(const uint8_t mac[6], uint32_t *day_id_out)
{
    uint32_t day_id = get_current_day_id(NULL);
    quota_record_t *record;

    if (day_id_out != NULL) {
        *day_id_out = day_id;
    }

    if (mac_is_zero(mac)) {
        return DAILY_QUOTA_SEC;
    }

    record = find_or_alloc_quota_record(mac, day_id);
    if (record == NULL || record->used_seconds >= DAILY_QUOTA_SEC) {
        return 0;
    }

    return (int)(DAILY_QUOTA_SEC - record->used_seconds);
}

static void checkpoint_session_usage(client_session_t *session, int64_t now, bool force_flush)
{
    uint32_t elapsed_seconds;
    uint32_t delta_seconds;
    quota_record_t *record;

    if (session == NULL || mac_is_zero(session->mac) || session->start_time <= 0 || session->granted_seconds == 0) {
        if (force_flush) {
            flush_quota_state(true);
        }
        return;
    }

    if (now <= session->start_time) {
        if (force_flush) {
            flush_quota_state(true);
        }
        return;
    }

    elapsed_seconds = (uint32_t)((now - session->start_time) / 1000000LL);
    if (elapsed_seconds > session->granted_seconds) {
        elapsed_seconds = session->granted_seconds;
    }

    if (elapsed_seconds <= session->accounted_seconds) {
        if (force_flush) {
            flush_quota_state(true);
        }
        return;
    }

    delta_seconds = elapsed_seconds - session->accounted_seconds;
    record = find_or_alloc_quota_record(session->mac,
                                        session->day_id != 0 ? session->day_id : get_current_day_id(NULL));
    if (record != NULL) {
        uint32_t new_used = record->used_seconds + delta_seconds;
        record->used_seconds = new_used > DAILY_QUOTA_SEC ? DAILY_QUOTA_SEC : new_used;
        s_quota_dirty = true;
    }

    session->accounted_seconds = elapsed_seconds;
    flush_quota_state(force_flush);
}

static void refresh_portal_auth_state(const char *reason)
{
    bool has_active_session = false;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            continue;
        }

        if (sessions[i].end_time > 0 && sessions[i].end_time <= now) {
            checkpoint_session_usage(&sessions[i], now, true);
            sessions[i].active = false;
            continue;
        }

        has_active_session = true;
    }

    if (s_global_session_end_time > 0 && s_global_session_end_time <= now) {
        s_global_session_end_time = 0;
    }

    portal_authenticated = has_active_session || (s_global_session_end_time > now);
    net_diag_set_portal_state(portal_authenticated, reason);
    if (!portal_authenticated) {
        maybe_disable_napt_if_idle(reason);
    }
}

static bool has_active_access(void)
{
    int64_t now = esp_timer_get_time();

    if (s_global_session_end_time > now) {
        return true;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && sessions[i].end_time > now) {
            return true;
        }
    }

    return false;
}

static void maybe_disable_napt_if_idle(const char *reason)
{
    esp_err_t err;

    if (!s_napt_enabled || has_active_access()) {
        return;
    }

    err = esp_netif_napt_disable(wifiAP);
    if (err == ESP_OK) {
        s_napt_enabled = false;
        ESP_LOGI(TAG_WEB, "NAPT disabled because no active sessions remain");
        net_diag_set_napt_state(false, ESP_OK);
        if (reason != NULL) {
            net_diag_schedule_probe(reason);
        }
        return;
    }

    ESP_LOGW(TAG_WEB, "Failed to disable NAPT on SoftAP netif: %s", esp_err_to_name(err));
    net_diag_set_napt_state(true, err);
}

static bool bytes_to_hex(const uint8_t *bytes, size_t byte_len, char *hex_out, size_t hex_out_len)
{
    if (bytes == NULL || hex_out == NULL || hex_out_len < ((byte_len * 2) + 1)) {
        return false;
    }

    for (size_t i = 0; i < byte_len; i++) {
        snprintf(&hex_out[i * 2], 3, "%02x", bytes[i]);
    }
    hex_out[byte_len * 2] = '\0';
    return true;
}

static void generate_session_token(char token_out[SESSION_TOKEN_LEN])
{
    uint8_t random_bytes[SESSION_TOKEN_HEX_LEN / 2];

    if (token_out == NULL) {
        return;
    }

    esp_fill_random(random_bytes, sizeof(random_bytes));
    if (!bytes_to_hex(random_bytes, sizeof(random_bytes), token_out, SESSION_TOKEN_LEN)) {
        token_out[0] = '\0';
    }
}

static void generate_hash_from_mac(const uint8_t mac[6], char device_hash_out[DEVICE_HASH_LEN])
{
    uint8_t digest[32];

    if (mac == NULL || device_hash_out == NULL) {
        return;
    }

    mbedtls_sha256(mac, 6, digest, 0);
    if (!bytes_to_hex(digest, sizeof(digest), device_hash_out, DEVICE_HASH_LEN)) {
        device_hash_out[0] = '\0';
    }
}

static void generate_hash_from_ip(uint32_t ip, char device_hash_out[DEVICE_HASH_LEN])
{
    uint8_t digest[32];
    uint8_t ip_bytes[4];

    if (device_hash_out == NULL) {
        return;
    }

    memcpy(ip_bytes, &ip, sizeof(ip_bytes));
    mbedtls_sha256(ip_bytes, sizeof(ip_bytes), digest, 0);
    if (!bytes_to_hex(digest, sizeof(digest), device_hash_out, DEVICE_HASH_LEN)) {
        device_hash_out[0] = '\0';
    }
}

static bool get_any_connected_client_mac(uint8_t mac_out[6])
{
    wifi_sta_list_t sta_list = {0};

    if (mac_out == NULL) {
        return false;
    }

    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK || sta_list.num <= 0) {
        return false;
    }

    memcpy(mac_out, sta_list.sta[0].mac, 6);
    return true;
}

static bool get_client_mac_for_ip(uint32_t ip, uint8_t mac_out[6])
{
    wifi_sta_list_t sta_list = {0};
    esp_netif_pair_mac_ip_t mac_ip_pair[MAX_CLIENTS] = {0};
    int pair_count;
    esp_err_t err;

    if (ip == 0 || mac_out == NULL) {
        return false;
    }

    err = esp_wifi_ap_get_sta_list(&sta_list);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_WEB, "Failed to get AP station list: %s", esp_err_to_name(err));
        return false;
    }

    pair_count = sta_list.num < MAX_CLIENTS ? sta_list.num : MAX_CLIENTS;
    if (pair_count <= 0) {
        return false;
    }

    for (int i = 0; i < pair_count; i++) {
        memcpy(mac_ip_pair[i].mac, sta_list.sta[i].mac, sizeof(mac_ip_pair[i].mac));
    }

    err = esp_netif_dhcps_get_clients_by_mac(wifiAP, pair_count, mac_ip_pair);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_WEB, "Failed to map DHCP clients to MAC addresses: %s", esp_err_to_name(err));
        return false;
    }

    for (int i = 0; i < pair_count; i++) {
        if (mac_ip_pair[i].ip.addr == ip) {
            memcpy(mac_out, mac_ip_pair[i].mac, 6);
            return true;
        }
    }

    return false;
}

static client_session_t *start_or_resume_session(uint32_t ip,
                                                 const uint8_t *mac,
                                                 const char *token,
                                                 const char *device_hash,
                                                 int granted_seconds)
{
    int64_t now = esp_timer_get_time();
    int session_index = -1;
    int session_duration_sec = granted_seconds > 0 ? granted_seconds : SESSION_DURATION_SEC;

    if (ip == 0) {
        s_global_session_end_time = now + ((int64_t)session_duration_sec * 1000000LL);
        refresh_portal_auth_state("portal_accept_global");
        if (token == NULL || token[0] == '\0') {
            ESP_LOGW(TAG_WEB, "Client IP lookup failed; using global session fallback");
            net_diag_log_snapshot("session_global_fallback");
            net_diag_schedule_probe("portal_accept");
            return NULL;
        }

        session_index = find_session_index_by_token(token);
    } else {
        if (mac != NULL) {
            session_index = find_session_index_by_mac(mac);
        }
        if (session_index < 0) {
            session_index = find_session_index_by_ip(ip);
        }
    }

    if (session_index >= 0) {
        if (sessions[session_index].end_time > now) {
            if (mac != NULL) {
                memcpy(sessions[session_index].mac, mac, 6);
            }
            if (token != NULL && token[0] != '\0' && sessions[session_index].token[0] == '\0') {
                strlcpy(sessions[session_index].token, token, sizeof(sessions[session_index].token));
            }
            if (device_hash != NULL && device_hash[0] != '\0') {
                strlcpy(sessions[session_index].device_hash, device_hash, sizeof(sessions[session_index].device_hash));
            }
            refresh_portal_auth_state("session_reused");
            ESP_LOGI(TAG_WEB, "Session exists for IP %lu. Keeping existing time.", ip);
            net_diag_log_snapshot("session_reused");
            return &sessions[session_index];
        }

        sessions[session_index].active = false;
    }

    if (mac != NULL) {
        session_index = find_session_index_by_mac(mac);
    }
    if (session_index < 0) {
        session_index = find_session_index_by_token(token);
    }
    if (session_index >= 0) {
        memset(sessions[session_index].mac, 0, sizeof(sessions[session_index].mac));
        sessions[session_index].token[0] = '\0';
        sessions[session_index].device_hash[0] = '\0';
        sessions[session_index].ip_addr = ip;
        if (mac != NULL) {
            memcpy(sessions[session_index].mac, mac, 6);
        }
        if (token != NULL && token[0] != '\0') {
            strlcpy(sessions[session_index].token, token, sizeof(sessions[session_index].token));
        }
        if (device_hash != NULL && device_hash[0] != '\0') {
            strlcpy(sessions[session_index].device_hash, device_hash, sizeof(sessions[session_index].device_hash));
        }
        sessions[session_index].start_time = now;
        sessions[session_index].granted_seconds = (uint32_t)session_duration_sec;
        sessions[session_index].accounted_seconds = 0;
        sessions[session_index].day_id = get_current_day_id(NULL);
        sessions[session_index].end_time = now + ((int64_t)session_duration_sec * 1000000LL);
        sessions[session_index].active = true;
        if (ip != 0) {
            s_global_session_end_time = 0;
        }
        refresh_portal_auth_state("session_resumed_by_token");
        net_diag_log_snapshot("session_resumed_by_token");
        net_diag_schedule_probe("portal_accept");
        return &sessions[session_index];
    }

    session_index = find_free_session_slot();
    if (session_index < 0) {
        session_index = 0;
        memset(&sessions[session_index], 0, sizeof(sessions[session_index]));
        net_diag_log_snapshot("session_overwrite_slot0");
    }

    memset(&sessions[session_index], 0, sizeof(sessions[session_index]));
    sessions[session_index].ip_addr = ip;
    sessions[session_index].start_time = now;
    sessions[session_index].granted_seconds = (uint32_t)session_duration_sec;
    sessions[session_index].accounted_seconds = 0;
    sessions[session_index].day_id = get_current_day_id(NULL);
    sessions[session_index].end_time = now + ((int64_t)session_duration_sec * 1000000LL);
    sessions[session_index].active = true;
    if (mac != NULL) {
        memcpy(sessions[session_index].mac, mac, 6);
    }
    if (token != NULL && token[0] != '\0') {
        strlcpy(sessions[session_index].token, token, sizeof(sessions[session_index].token));
    }
    if (device_hash != NULL && device_hash[0] != '\0') {
        strlcpy(sessions[session_index].device_hash, device_hash, sizeof(sessions[session_index].device_hash));
    }

    if (ip != 0) {
        s_global_session_end_time = 0;
    }
    refresh_portal_auth_state("session_started");
    ESP_LOGI(TAG_WEB, "Starting session for IP: %lu", ip);
    net_diag_log_snapshot("session_started");
    net_diag_schedule_probe("portal_accept");
    return &sessions[session_index];
}

static esp_err_t ensure_napt_enabled(void)
{
    if (s_napt_enabled) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_napt_enable(wifiAP);
    if (err == ESP_OK) {
        s_napt_enabled = true;
        ESP_LOGI(TAG_WEB, "NAPT enabled on SoftAP netif");
        net_diag_set_napt_state(true, ESP_OK);
        return ESP_OK;
    }

    ESP_LOGE(TAG_WEB, "Failed to enable NAPT on SoftAP netif: %s", esp_err_to_name(err));
    net_diag_set_napt_state(false, err);
    net_diag_schedule_probe("napt_enable_failed");
    return err;
}

static void revoke_session(int session_index, bool disconnected, const char *reason)
{
    char token[SESSION_TOKEN_LEN] = {0};
    int64_t now = esp_timer_get_time();

    if (session_index < 0 || session_index >= MAX_CLIENTS || !sessions[session_index].active) {
        return;
    }

    checkpoint_session_usage(&sessions[session_index], now, true);

    if (sessions[session_index].token[0] != '\0') {
        strlcpy(token, sessions[session_index].token, sizeof(token));
    }

    sessions[session_index].active = false;
    refresh_portal_auth_state(reason);

    if (token[0] == '\0') {
        return;
    }

    if (disconnected) {
        esp_err_t err = supabase_mark_disconnected(token);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_WEB, "Failed to mark session disconnected in Supabase: %s",
                     esp_err_to_name(err));
        }
    } else {
        esp_err_t err = supabase_update_heartbeat(token, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_WEB, "Failed to mark session expired in Supabase: %s",
                     esp_err_to_name(err));
        }
    }
}

static int get_global_remaining_seconds(void)
{
    int64_t now = esp_timer_get_time();
    int64_t remaining = s_global_session_end_time - now;

    if (remaining <= 0) {
        s_global_session_end_time = 0;
        refresh_portal_auth_state("global_session_expired");
        return -1;
    }

    return (int)(remaining / 1000000LL);
}

uint32_t get_client_ip(httpd_req_t *req) {
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
        return addr.sin_addr.s_addr;
    }
    ESP_LOGW(TAG_WEB, "Failed to get client IP for sockfd %d, errno=%d", sockfd, errno);
    return 0;
}

int get_remaining_seconds(uint32_t ip) {
    int64_t now = esp_timer_get_time();
    uint8_t client_mac[6] = {0};

    if (ip != 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].active && sessions[i].ip_addr == ip) {
                int64_t remaining = sessions[i].end_time - now;
                if (remaining <= 0) {
                    revoke_session(i, false, "session_expired");
                    return -1;
                }
                return (int)(remaining / 1000000LL);
            }
        }

        if (get_client_mac_for_ip(ip, client_mac)) {
            int session_index = find_session_index_by_mac(client_mac);
            if (session_index >= 0) {
                int64_t remaining = sessions[session_index].end_time - now;
                if (remaining <= 0) {
                    revoke_session(session_index, false, "session_expired_by_mac");
                    return -1;
                }

                sessions[session_index].ip_addr = ip;
                return (int)(remaining / 1000000LL);
            }
        }
    }

    return get_global_remaining_seconds();
}

bool is_client_session_active(uint32_t ip_addr)
{
    return get_remaining_seconds(ip_addr) > 0;
}

void start_session(uint32_t ip) {
    start_or_resume_session(ip, NULL, NULL, NULL, SESSION_DURATION_SEC);
}

void handle_client_disconnect(const uint8_t mac[6])
{
    if (mac == NULL) {
        return;
    }

    ESP_LOGI(TAG_WEB, "Handling AP disconnect for client session");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            continue;
        }

        if (memcmp(sessions[i].mac, mac, 6) == 0) {
            revoke_session(i, true, "ap_client_disconnected");
        }
    }
}

static void supabase_heartbeat_task(void *pvParameters)
{
    (void)pvParameters;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int remaining;

            if (!sessions[i].active || sessions[i].token[0] == '\0') {
                continue;
            }

            checkpoint_session_usage(&sessions[i], esp_timer_get_time(), false);
            remaining = get_remaining_seconds(sessions[i].ip_addr);
            if (remaining > 0) {
                esp_err_t err = supabase_update_heartbeat(sessions[i].token, remaining);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG_WEB, "Heartbeat failed for a session: %s",
                             esp_err_to_name(err));
                }
                continue;
            }

            ESP_LOGI(TAG_WEB, "Session expired");
            revoke_session(i, false, "session_expired_heartbeat");
        }
    }
}

static void ensure_heartbeat_task_started(void)
{
    if (s_heartbeat_task_started) {
        return;
    }

    if (xTaskCreate(supabase_heartbeat_task,
                    "supabase_heartbeat",
                    6144,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    NULL) == pdPASS) {
        s_heartbeat_task_started = true;
        ESP_LOGI(TAG_WEB, "Supabase heartbeat task started");
    } else {
        ESP_LOGE(TAG_WEB, "Failed to start Supabase heartbeat task");
    }
}

char* html_escape(const char* src) {
    int len = strlen(src);
    int esc_len = len + 1;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' || src[i] == '\'' || src[i] == '\"' || src[i] == '&' || src[i] == '#' || src[i] == ';') esc_len += 4;
    }
    char* res = malloc(sizeof(char) * esc_len);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' || src[i] == '\'' || src[i] == '\"' || src[i] == '&' || src[i] == '#' || src[i] == ';') {
            res[j++] = '&'; res[j++] = '#'; res[j++] = '0' + (src[i] / 10); res[j++] = '0' + (src[i] % 10); res[j++] = ';';
        } else {
            res[j++] = src[i];
        }
    }
    res[j] = '\0';
    return res;
}

/* API Endpoint (Optional fallback) */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    uint32_t ip = get_client_ip(req);
    int remaining_seconds = get_remaining_seconds(ip);
    bool is_connected = (remaining_seconds > 0);
    if (remaining_seconds < 0) remaining_seconds = 0;

    char resp_buf[100]; 
    snprintf(resp_buf, sizeof(resp_buf), "{\"authenticated\": %s, \"remaining_seconds\": %d}", is_connected ? "true" : "false", remaining_seconds);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); 
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_send(req, resp_buf, strlen(resp_buf));
    return ESP_OK;
}
// --- INSERT THIS HELPER FUNCTION HERE ---
typedef enum {
    AUTH_OK = 0,
    AUTH_MISSING = 1,
    AUTH_WRONG = 2
} auth_status_t;

static auth_status_t check_auth(httpd_req_t *req) {
    char *buf = NULL;
    size_t buf_len = 0;
    
    buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len <= 1) return AUTH_MISSING;

    buf = malloc(buf_len);
    if (!buf) return AUTH_MISSING;
    
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) != ESP_OK) {
        free(buf);
        return AUTH_MISSING;
    }

    if (strncmp(buf, "Basic ", 6) != 0) {
        free(buf);
        return AUTH_MISSING;
    }

    char *encoded = buf + 6; 
    unsigned char decoded[64]; 
    size_t decoded_len = 0;
    
    mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len, 
                         (const unsigned char *)encoded, strlen(encoded));
    decoded[decoded_len] = '\0'; 
    free(buf); 

    char expected[64];
    snprintf(expected, sizeof(expected), "%s:%s", ADMIN_DEFAULT_USERNAME, ADMIN_DEFAULT_PASSWORD);

    if (strcmp((char *)decoded, expected) == 0) return AUTH_OK;

    return AUTH_WRONG;
}
// ----------------------------------------

static char *receive_form_body(httpd_req_t *req)
{
    char *body;
    int received = 0;
    int ret;

    if (req->content_len <= 0) {
        return NULL;
    }

    body = malloc((size_t)req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }

    while (received < req->content_len) {
        ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return NULL;
        }
        received += ret;
    }
    body[received] = '\0';
    return body;
}

static bool get_form_value(char *body, const char *key, char *out, size_t out_len)
{
    if (body == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    if (httpd_query_key_value(body, key, out, out_len) != ESP_OK) {
        return false;
    }
    preprocess_string(out);
    return true;
}

static char *load_config_string_or_default(const char *key, const char *default_value)
{
    char *value = NULL;

    if (get_config_param_str((char *)key, &value) == ESP_OK && value != NULL) {
        return value;
    }

    value = malloc(strlen(default_value) + 1);
    if (value != NULL) {
        strcpy(value, default_value);
    }
    return value;
}

static esp_err_t send_admin_auth_failure(httpd_req_t *req)
{
    const char* redirect_html = 
        "<html><head>"
        "<meta http-equiv='refresh' content='3;url=/' />"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<style>body{font-family:sans-serif;text-align:center;padding:50px;background:#F5F0EB;color:#521B1B;}</style>"
        "</head><body>"
        "<h1>Access Denied</h1>"
        "<p>Authorization failed or was cancelled.</p>"
        "<p>Redirecting to main page...</p>"
        "<script>setTimeout(function(){ window.location.href='/'; }, 3000);</script>"
        "</body></html>";

        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"" AUTH_REALM "\"");
        httpd_resp_send(req, redirect_html, strlen(redirect_html));
    return ESP_OK;
}

static esp_err_t send_admin_result(httpd_req_t *req, const char *title, const char *message)
{
    char response[768];

    snprintf(response, sizeof(response),
             "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
             "<style>body{font-family:sans-serif;text-align:center;padding:50px;background:#F5F0EB;color:#521B1B;}"
             "a{color:#521B1B;font-weight:bold;}</style></head>"
             "<body><h1>%s</h1><p>%s</p><a href='/config'>Return to config</a></body></html>",
             title, message);
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Admin Config Handler */
static esp_err_t admin_config_get_handler(httpd_req_t *req)
{
    auth_status_t status = check_auth(req);

    if (status != AUTH_OK) {
        return send_admin_auth_failure(req);
    }

    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

static esp_err_t admin_config_post_handler(httpd_req_t *req)
{
    char *body;
    char action[32] = {0};
    esp_err_t err = ESP_OK;

    if (check_auth(req) != AUTH_OK) {
        return send_admin_auth_failure(req);
    }

    body = receive_form_body(req);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (!get_form_value(body, "action", action, sizeof(action))) {
        free(body);
        return send_admin_result(req, "Invalid Request", "No config action was provided.");
    }

    if (strcmp(action, "reboot") == 0) {
        esp_timer_start_once(restart_timer, 500000);
        free(body);
        return send_admin_result(req, "Rebooting", "The device is rebooting.");
    }

    if (strcmp(action, "save_ap") == 0) {
        char ap_ssid_value[64] = {0};
        char ap_password_value[64] = {0};
        char *stored_ap_password = load_config_string_or_default("ap_passwd", "");
        char *argv[] = {"set_ap", ap_ssid_value, ap_password_value};

        get_form_value(body, "ap_ssid", ap_ssid_value, sizeof(ap_ssid_value));
        get_form_value(body, "ap_password", ap_password_value, sizeof(ap_password_value));
        if (ap_password_value[0] == '\0' && stored_ap_password != NULL) {
            strlcpy(ap_password_value, stored_ap_password, sizeof(ap_password_value));
        }
        err = (esp_err_t)set_ap(3, argv);
        free(stored_ap_password);
    } else if (strcmp(action, "save_sta") == 0) {
        char sta_ssid_value[64] = {0};
        char sta_password_value[64] = {0};
        char ent_username_value[64] = {0};
        char ent_identity_value[64] = {0};
        char *stored_sta_password = load_config_string_or_default("passwd", "");
        char *argv[7];
        int argc = 3;

        get_form_value(body, "ssid", sta_ssid_value, sizeof(sta_ssid_value));
        get_form_value(body, "password", sta_password_value, sizeof(sta_password_value));
        get_form_value(body, "ent_username", ent_username_value, sizeof(ent_username_value));
        get_form_value(body, "ent_identity", ent_identity_value, sizeof(ent_identity_value));
        if (sta_password_value[0] == '\0' && stored_sta_password != NULL) {
            strlcpy(sta_password_value, stored_sta_password, sizeof(sta_password_value));
        }

        argv[0] = "set_sta";
        argv[1] = sta_ssid_value;
        argv[2] = sta_password_value;
        if (ent_username_value[0] != '\0') {
            argv[argc++] = "-u";
            argv[argc++] = ent_username_value;
        }
        if (ent_identity_value[0] != '\0') {
            argv[argc++] = "-a";
            argv[argc++] = ent_identity_value;
        }

        err = (esp_err_t)set_sta(argc, argv);
        free(stored_sta_password);
    } else if (strcmp(action, "save_static") == 0) {
        char static_ip_value[32] = {0};
        char subnet_mask_value[32] = {0};
        char gateway_value[32] = {0};
        char *argv[] = {"set_sta_static", static_ip_value, subnet_mask_value, gateway_value};

        get_form_value(body, "staticip", static_ip_value, sizeof(static_ip_value));
        get_form_value(body, "subnetmask", subnet_mask_value, sizeof(subnet_mask_value));
        get_form_value(body, "gateway", gateway_value, sizeof(gateway_value));
        err = (esp_err_t)set_sta_static(4, argv);
    } else {
        free(body);
        return send_admin_result(req, "Unknown Action", "This config action is not supported.");
    }

    free(body);
    if (err != ESP_OK) {
        return send_admin_result(req, "Configuration Failed", "The requested settings could not be stored.");
    }
    esp_timer_start_once(restart_timer, 500000);
    return send_admin_result(req, "Settings Applied", "The device is rebooting to apply the new configuration.");
}

/* * PORTAL HANDLER (/)
 * The Login Page.
 * "Start" Button now links directly to /confirm
 */
static esp_err_t portal_handler(httpd_req_t *req)
{
    // Check if user is already authenticated
    uint32_t ip = get_client_ip(req);
    int remaining = get_remaining_seconds(ip);

    if (remaining > 0) {
        // Already logged in? Go to confirm page directly
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/confirm");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // Serve the Login HTML
    const char* resp_str = 
        "<html><head><title>CPE Wi-Fi</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<style>"
        /* Body now centers the card in the middle of the screen */
        " body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: #F5F0EB; color: #521B1B; display: flex; align-items: center; justify-content: center; min-height: 100vh; margin: 0; padding: 20px; box-sizing: border-box; }"
        
        /* NEW: Card container for Laptop/Desktop view */
        " .card { width: 100%%; max-width: 420px; background-color: #ffffff; padding: 40px; border-radius: 24px; box-shadow: 0 10px 40px rgba(82, 27, 27, 0.1); display: flex; flex-direction: column; align-items: center; }"
        
        " .icon { width: 80px; height: 80px; fill: #521B1B; margin-bottom: 20px; }"
        " h2 { font-size: 20px; font-weight: normal; color: #6D3B39; margin: 0; text-align: center; }"
        " h1 { font-size: 28px; font-weight: bold; margin: 5px 0 30px 0; text-align: center; }"
        " hr { width: 100%%; border: 0; border-top: 1px solid #E0D8D0; margin: 0 0 20px 0; }"
        " .content { width: 100%%; }"
        " h3 { font-size: 16px; font-weight: bold; margin-bottom: 10px; color: #521B1B; }"
        " p { font-size: 13px; line-height: 1.6; color: #6D3B39; margin-bottom: 25px; text-align: justify; }"
        
        " .checkbox-container { display: flex; align-items: center; font-size: 13px; font-weight: bold; margin-bottom: 30px; color: #521B1B; width: 100%%; background: #F9F6F3; padding: 10px; border-radius: 8px; box-sizing: border-box; }"
        " input[type='checkbox'] { transform: scale(1.3); margin-right: 12px; accent-color: #521B1B; cursor: pointer; }"
        
        " .btn { display: block; width: 100%%; padding: 14px 0; font-size: 16px; font-weight: bold; color: #fff; background-color: #1D734B; border: none; border-radius: 12px; text-decoration: none; text-align: center; cursor: pointer; transition: all 0.2s ease; }"
        " .btn:hover { background-color: #145235; transform: translateY(-1px); }"
        " .btn.loading { background-color: #6D3B39; pointer-events: none; }"
        " .status { display: none; margin-top: 14px; font-size: 13px; color: #6D3B39; text-align: center; }"
        "</style>"
        "<script>"
        " function toggleButton() {"
        "   var checkBox = document.getElementById('agree');"
        "   var btn = document.getElementById('connectBtn');"
        "   if (checkBox.checked){ btn.style.pointerEvents = 'auto'; btn.style.opacity = '1'; } "
        "   else { btn.style.pointerEvents = 'none'; btn.style.opacity = '0.5'; }"
        " }"
        " function startConnecting(event) {"
        "   event.preventDefault();"
        "   var btn = document.getElementById('connectBtn');"
        "   var status = document.getElementById('connectingStatus');"
        "   if (btn.style.pointerEvents === 'none') { return false; }"
        "   btn.classList.add('loading');"
        "   btn.textContent = 'Connecting...';"
        "   status.style.display = 'block';"
        "   window.setTimeout(function(){ window.location.href='/confirm'; }, 700);"
        "   return false;"
        " }"
        "</script>"
        "</head>"
        "<body>"
        /* Wrapped everything in the new .card div */
        " <div class='card'>"
        "   <svg class='icon' viewBox='0 0 24 24'>"
        "    <path d='M12 21L12 21C11.05 21 10.24 20.43 9.85 19.63L12 17L14.15 19.63C13.76 20.43 12.95 21 12 21ZM12 3C7.95 3 4.21 4.34 1.2 6.6L3 9C5.5 7.12 8.62 6 12 6C15.38 6 18.5 7.12 21 9L22.8 6.6C19.79 4.34 16.05 3 12 3ZM12 9C9.3 9 6.81 9.89 4.8 11.4L6.6 13.8C8.1 12.67 9.97 12 12 12C14.03 12 15.9 12.67 17.4 13.8L19.2 11.4C17.19 9.89 14.7 9 12 9Z'/>"
        "   </svg>"
        "   <h2>You are connecting to</h2>"
        "   <h1>&lsquo;SOLAR CONNECT&rsquo;</h1>"
        "   <div class='content'>"
        "    <hr>"
        "    <h3>Terms and Conditions</h3>"
        "    <p>By connecting to this Wi-Fi network, you agree to use the service solely for educational purposes. Each device is allowed a maximum connection time of <strong>1 hour per day</strong>.</p>"
        "    <div class='checkbox-container'>"
        "     <input type='checkbox' id='agree' onclick='toggleButton()'>"
        "     <label for='agree'>I agree to the Terms.</label>"
        "    </div>"
        "    <a href='/confirm' id='connectBtn' class='btn' onclick='return startConnecting(event)' style='pointer-events: none; opacity: 0.5;'>Start Browsing</a>"
        "    <div id='connectingStatus' class='status'>Connecting to the internet. Please wait...</div>"
        "   </div>"
        " </div>"
        "</body></html>";

    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

/* * REDIRECT HANDLER
 * Hijacks DNS checks and sends to Portal (/) if not authenticated.
 * If authenticated, sends 404 to satisfy OS "Connectivity Check".
 */
static esp_err_t send_redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(wifiAP, &ip_info);
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    
    char redirect_url[64];
    snprintf(redirect_url, sizeof(redirect_url), "http://%s/", ip_str);
    httpd_resp_set_hdr(req, "Location", redirect_url);
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t send_probe_success(httpd_req_t *req)
{
    if (strcmp(req->uri, "/generate_204") == 0 || strcmp(req->uri, "/gen_204") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strcmp(req->uri, "/connecttest.txt") == 0 || strcmp(req->uri, "/ncsi.txt") == 0) {
        static const char windows_probe[] = "Microsoft Connect Test";
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, windows_probe, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    static const char apple_probe[] =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, apple_probe, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    if (is_client_session_active(get_client_ip(req))) {
        return send_probe_success(req);
    }

    return send_redirect_to_portal(req);
}

static esp_err_t catch_all_handler(httpd_req_t *req)
{
    if (is_client_session_active(get_client_ip(req))) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    return send_redirect_to_portal(req);
}

static esp_err_t send_quota_page(httpd_req_t *req)
{
    char response[2048];

    snprintf(response, sizeof(response),
             "<!DOCTYPE html><html><head><title>Daily Time Used Up</title>"
             "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
             "<style>"
             "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; "
             "background: #F5F0EB; color: #521B1B; display: flex; align-items: center; justify-content: center; "
             "min-height: 100vh; margin: 0; padding: 24px; box-sizing: border-box; }"
             ".card { width: 100%%; max-width: 440px; background: #fff; border-radius: 24px; padding: 32px; "
             "box-shadow: 0 10px 40px rgba(82, 27, 27, 0.1); text-align: center; }"
             "h1 { font-size: 28px; margin: 0 0 12px; }"
             "p { line-height: 1.6; color: #6D3B39; margin: 0 0 18px; }"
             ".btn { display: inline-block; margin-top: 8px; padding: 14px 20px; background: #1D734B; color: #fff; "
             "text-decoration: none; border-radius: 12px; font-weight: 700; }"
             "</style></head><body><div class='card'>"
             "<h1>Today's 1 hour is already used up</h1>"
             "<p>Your device has reached its daily internet limit on SOLAR CONNECT.</p>"
             "<p>You can still open the app now, and you can connect to the internet again tomorrow.</p>"
             "<a class='btn' href='%s' target='_blank'>Open Solar Connect App</a>"
             "</div></body></html>",
             PWA_DASHBOARD_URL);

    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* * CONFIRM HANDLER (/confirm)
 * 1. Enables Internet & Starts Timer (if not already started)
 * 2. Shows "You are connected"
 * 3. Generates the button that passes the timer data to Vercel
 */
static esp_err_t confirm_handler(httpd_req_t *req)
{
    uint32_t ip = get_client_ip(req);
    uint8_t client_mac[6] = {0};
    bool have_mac = false;
    char token[SESSION_TOKEN_LEN] = {0};
    char device_hash[DEVICE_HASH_LEN] = {0};
    client_session_t *session;
    const char *session_token;
    const char *session_device_hash;
    const char *dashboard_url;
    char dashboard_url_buf[192];
    char remaining_label[64];
    int granted_seconds = SESSION_DURATION_SEC;
    net_diag_log_snapshot("confirm_handler");

    have_mac = get_client_mac_for_ip(ip, client_mac);
    generate_session_token(token);

    if (have_mac) {
        generate_hash_from_mac(client_mac, device_hash);
    } else if (ip != 0) {
        ip4_addr_t ip_addr = { .addr = ip };
        generate_hash_from_ip(ip, device_hash);
        ESP_LOGW(TAG_WEB, "Falling back to IP-derived device hash for client " IPSTR, IP2STR(&ip_addr));
    } else {
        ESP_LOGW(TAG_WEB, "Unable to derive device hash for /confirm; dashboard link will omit token");
    }

    if (have_mac) {
        granted_seconds = get_quota_remaining_for_mac(client_mac, NULL);
        if (granted_seconds <= 0) {
            ESP_LOGI(TAG_WEB, "Daily quota exhausted for this device");
            return send_quota_page(req);
        }

        {
            int seconds_until_next_day = get_seconds_until_next_day();
            if (seconds_until_next_day < granted_seconds) {
                granted_seconds = seconds_until_next_day;
            }
        }
    }

    // --- START THE INTERNET HERE ---
    session = start_or_resume_session(ip,
                                      have_mac ? client_mac : NULL,
                                      token[0] != '\0' ? token : NULL,
                                      device_hash[0] != '\0' ? device_hash : NULL,
                                      granted_seconds);

    if (!ap_connect) {
        const char *waiting_html =
            "<!DOCTYPE html><html><head><title>Connecting...</title>"
            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
            "<meta http-equiv='refresh' content='3;url=/confirm'>"
            "<style>"
            "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; "
            "background: #F5F0EB; color: #521B1B; display: flex; align-items: center; justify-content: center; "
            "min-height: 100vh; margin: 0; padding: 24px; box-sizing: border-box; }"
            ".card { width: 100%; max-width: 420px; background: #fff; border-radius: 24px; padding: 32px; "
            "box-shadow: 0 10px 40px rgba(82, 27, 27, 0.1); text-align: center; }"
            "h1 { font-size: 26px; margin: 0 0 12px; }"
            "p { line-height: 1.6; margin: 0; }"
            "</style></head><body>"
            "<div class='card'>"
            "<h1>Connecting to the internet...</h1>"
            "<p>Your device is authenticated, but the ESP32 is still connecting to the upstream Wi-Fi network. "
            "This page will retry automatically in a few seconds.</p>"
            "</div></body></html>";

        httpd_resp_send(req, waiting_html, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (ensure_napt_enabled() != ESP_OK) {
        const char *error_html =
            "<!DOCTYPE html><html><head><title>Routing Error</title>"
            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
            "<style>"
            "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; "
            "background: #F5F0EB; color: #521B1B; display: flex; align-items: center; justify-content: center; "
            "min-height: 100vh; margin: 0; padding: 24px; box-sizing: border-box; }"
            ".card { width: 100%; max-width: 420px; background: #fff; border-radius: 24px; padding: 32px; "
            "box-shadow: 0 10px 40px rgba(82, 27, 27, 0.1); text-align: center; }"
            "h1 { font-size: 26px; margin: 0 0 12px; }"
            "p { line-height: 1.6; margin: 0; }"
            "</style></head><body>"
            "<div class='card'>"
            "<h1>Internet routing is not available yet</h1>"
            "<p>The ESP32 authenticated your device but failed to enable NAT routing to the uplink. "
            "Check the serial logs for the NAPT error.</p>"
            "</div></body></html>";

        httpd_resp_send(req, error_html, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    // DNS stays pointed at the ESP32 so we can hijack unauthenticated lookups
    // and forward authenticated ones upstream.
    // -------------------------------

    int remaining_seconds = get_remaining_seconds(ip);
    if (remaining_seconds < 0) remaining_seconds = 0;
    snprintf(remaining_label, sizeof(remaining_label),
             "%d minute%s remaining today",
             (remaining_seconds + 59) / 60,
             ((remaining_seconds + 59) / 60) == 1 ? "" : "s");

    session_token = (session != NULL && session->token[0] != '\0') ? session->token : (token[0] != '\0' ? token : NULL);
    session_device_hash = (session != NULL && session->device_hash[0] != '\0') ? session->device_hash : (device_hash[0] != '\0' ? device_hash : NULL);

    if (session_token != NULL && session_device_hash != NULL) {
        ESP_LOGI(TAG_WEB, "Syncing portal session to Supabase");
        esp_err_t sync_err = supabase_create_session(session_token,
                                                     session_device_hash,
                                                     remaining_seconds > 0 ? remaining_seconds : granted_seconds);
        if (sync_err == ESP_ERR_INVALID_STATE) {
            sync_err = supabase_update_heartbeat(session_token, remaining_seconds);
        }

        if (sync_err != ESP_OK) {
            ESP_LOGW(TAG_WEB, "Failed to sync session to Supabase: %s",
                     esp_err_to_name(sync_err));
        }
    } else {
        ESP_LOGW(TAG_WEB, "Skipping Supabase sync because session token or device hash is missing");
    }

    if (session != NULL && session->token[0] != '\0') {
        snprintf(dashboard_url_buf, sizeof(dashboard_url_buf),
                 PWA_LINK_URL_FORMAT, session->token);
        dashboard_url = dashboard_url_buf;
    } else if (token[0] != '\0') {
        snprintf(dashboard_url_buf, sizeof(dashboard_url_buf),
                 PWA_LINK_URL_FORMAT, token);
        dashboard_url = dashboard_url_buf;
    } else {
        dashboard_url = PWA_DASHBOARD_URL;
    }

    // Increased buffer size for the advanced responsive CSS
    char *resp_str = malloc(8192); 
    if (resp_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(resp_str, 8192,
    "<!DOCTYPE html><html><head><title>Connected to 'SOLAR CONNECT'</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<style>"
    "body { font-family: Inter, sans-serif; text-align: left; background-color: white; margin: 0; padding: 0; color: #41341E; }"
    ".hero { width: 100%%; height: 40vh; object-fit: cover; display: block; mask-image: linear-gradient(to bottom, black 60%%, transparent 100%%); -webkit-mask-image: linear-gradient(to bottom, black 60%%, transparent 100%%); }"
    ".container { width: 100%%; max-width: 800px; margin: -60px auto 40px; padding: 0 15px; position: relative; z-index: 10; box-sizing: border-box; }"
    ".main-card { background: #F5EFE6; padding: 25px; border-radius: 24px; height: 420px; overflow: hidden; width: 95%%; max-width: 550px; margin: 0 auto; box-sizing: border-box; position: relative; box-shadow: 0 10px 30px rgba(0,0,0,0.08); transition: height 0.3s ease; }"
    ".main-card::after { content: ''; position: absolute; bottom: 0; left: 0; right: 0; height: 100px; background: linear-gradient(to bottom, rgba(245, 239, 230, 0), #F5EFE6); pointer-events: none; z-index: 2; }"
    "h1 { color: #6F1D1B; font-size: clamp(20px, 4.5vw, 26px); font-weight: 400; margin: 0; line-height: 1.2; }"
    "h1 b { font-weight: 700; display: block; }"
    ".quota-pill { display: inline-block; margin: 14px 0 4px; padding: 8px 14px; border-radius: 999px; "
    "background: #E9F4ED; color: #1D734B; font-size: 13px; font-weight: 700; }"
    "hr { border: 0; height: 1px; background-color: #E0D8D0; margin: 18px 0; }"
    ".grid { display: flex; gap: 12px; align-items: flex-start; }"
    ".left-col { flex: 1; }"
    ".right-col { flex: 1.2; display: flex; flex-direction: column; align-items: stretch; }"
    "img.p-img, img.d-img { width: 100%%; height: auto; border-radius: 12px; display: block; }"
    "@media (max-width: 400px) { .right-col { width: 142px; flex: none; } .left-col { width: 142px; flex: none; } .main-card { height: 320px; } }"
    "@media (max-width: 320px) { .grid { flex-direction: column; gap: 20px; } .right-col { order: 1; width: 100%%; } .left-col { order: 2; width: 100%%; } .main-card { height: 580px; } }"
    "h2 { font-size: clamp(12px, 2.5vw, 12px); font-weight: 700; margin: 0 0 10px 2px; width: 100%%; }"
    ".btn { font-size: clamp(10px, 2vw, 13px); display: block; background-color: #1A1A1A; color: white; text-decoration: none; padding: clamp(8px, 1.5vw, 10px) 0; text-align: center; border-radius: 8px; font-weight: 600; margin-bottom: 11px; margin-left: 2px; width: 85%%; box-sizing: border-box; white-space: nowrap; }"
    "</style>"
    "</head><body>"
    "<img src='/cea.png' class='hero' />"
    "<div class='container'>"
    "  <div class='main-card'>"
    "    <h1>You are now connected to<br><b>'SOLAR CONNECT'</b></h1>"
    "    <div class='quota-pill'>%s</div>"
    "    <hr>"
    "    <div class='grid'>"
    "      <div class='left-col'>"
    "        <img src='/dashboard.png' class='p-img'/>"
    "      </div>"
    "      <div class='right-col'>"
    "        <h2>Explore the dashboard</h2>"
    "        <a href='%s' target='_blank' class='btn'>Solar-Powered Bench</a>"
    "        <img src='/dashboard-ui.png' class='d-img'/>"
    "      </div>"
    "    </div>"
    "  </div>"
    "</div>"
    "</body></html>",
    remaining_label,
    dashboard_url
    );

    httpd_resp_send(req, resp_str, strlen(resp_str));
    free(resp_str);
    return ESP_OK;
}

/* Image Handler */
static esp_err_t img_handler(httpd_req_t *req)
{
    char filepath[600]; 
    snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    FILE *f = fopen(filepath, "r");
    if (f == NULL) { httpd_resp_send_404(req); return ESP_FAIL; }
    
    if (strstr(req->uri, ".jpg") || strstr(req->uri, ".jpeg")) httpd_resp_set_type(req, "image/jpeg");
    else httpd_resp_set_type(req, "image/png");

    char chunk[1024]; size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    return ESP_OK;
}

httpd_handle_t start_webserver(void)
{
    init_sessions();
    supabase_init();
    ensure_heartbeat_task_started();
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_timer_create(&restart_timer_args, &restart_timer);

    const char* config_page_template = CONFIG_PAGE;
    char* safe_ap_ssid = html_escape(ap_ssid);
    char* safe_ssid = html_escape(ssid);
    char* safe_ent_username = html_escape(ent_username); char* safe_ent_identity = html_escape(ent_identity);
    int page_len = strlen(config_page_template) + strlen(safe_ap_ssid) +
                   strlen(safe_ssid) + strlen(safe_ent_username) + 
                   strlen(safe_ent_identity) + 256;
    char* config_page = malloc(sizeof(char) * page_len);
    snprintf(config_page, page_len, config_page_template,
        safe_ap_ssid, safe_ssid, safe_ent_username, safe_ent_identity,
        static_ip, subnet_mask, gateway_addr);
    free(safe_ap_ssid); free(safe_ssid);
    free(safe_ent_username); free(safe_ent_identity);

    if (httpd_start(&server, &config) != ESP_OK) return NULL;

    httpd_uri_t api_status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_status_uri);
    httpd_uri_t portal_uri = { .uri = "/", .method = HTTP_GET, .handler = portal_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &portal_uri);
    
    httpd_uri_t confirm_uri = { .uri = "/confirm", .method = HTTP_GET, .handler = confirm_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &confirm_uri);
    httpd_uri_t admin_get_uri = { .uri = "/config", .method = HTTP_GET, .handler = admin_config_get_handler, .user_ctx = config_page };
    httpd_register_uri_handler(server, &admin_get_uri);
    httpd_uri_t admin_post_uri = { .uri = "/config", .method = HTTP_POST, .handler = admin_config_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &admin_post_uri);
    httpd_uri_t gen_204_uri = { .uri = "/generate_204", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &gen_204_uri);
    httpd_uri_t gen_204_alt_uri = { .uri = "/gen_204", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &gen_204_alt_uri);
    httpd_uri_t connectivity_check_uri = { .uri = "/connectivity-check.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &connectivity_check_uri);
    httpd_uri_t hotspot_uri = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &hotspot_uri);
    httpd_uri_t apple_success_uri = { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &apple_success_uri);
    httpd_uri_t windows_connecttest_uri = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &windows_connecttest_uri);
    httpd_uri_t windows_ncsi_uri = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &windows_ncsi_uri);
    httpd_uri_t windows_redirect_uri = { .uri = "/redirect", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &windows_redirect_uri);
    httpd_uri_t microsoft_fwlink_uri = { .uri = "/fwlink", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &microsoft_fwlink_uri);
    httpd_uri_t cea_uri = { .uri = "/cea.png", .method = HTTP_GET, .handler = img_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &cea_uri);
    httpd_uri_t phone_uri = { .uri = "/dashboard.png", .method = HTTP_GET, .handler = img_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &phone_uri);
    httpd_uri_t dash_uri = { .uri = "/dashboard-ui.png", .method = HTTP_GET, .handler = img_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &dash_uri);
    httpd_uri_t catch_all_uri = { .uri = "/*", .method = HTTP_GET, .handler = catch_all_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &catch_all_uri);
    httpd_uri_t catch_all_head_uri = { .uri = "/*", .method = HTTP_HEAD, .handler = catch_all_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &catch_all_head_uri);

    return server;
}
