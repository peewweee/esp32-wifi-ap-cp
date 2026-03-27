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
#include "mbedtls/base64.h" // Required for decoding the password
#include "mbedtls/sha256.h"

static const char *TAG_WEB = "CAPTIVE_PORTAL";
static const char *TAG_ADMIN = "ADMIN_SERVER";

// --- ADMIN CREDENTIALS ---
#define ADMIN_USERNAME "admin"
#define ADMIN_PASSWORD "admin123"
#define AUTH_REALM "Admin Configuration"

// --- SESSION CONFIGURATION ---
#define MAX_CLIENTS 20
// TIMER SET TO 60 SECONDS FOR TESTING
#define SESSION_DURATION_SEC 60 
#define SESSION_DURATION_US (SESSION_DURATION_SEC * 1000000LL)

// --- PER-DEVICE SESSION STRUCTURE ---
typedef struct {
    uint32_t ip_addr;       // The client's IP address
    uint8_t mac[6];         // The client's MAC address
    char token[17];         // The Supabase/dashboard token
    char mac_hash[65];      // Full SHA-256 hex digest used for Supabase
    int64_t end_time;       // When their session expires (internal ESP time)
    bool active;            // Is this slot in use?
} client_session_t;

static client_session_t sessions[MAX_CLIENTS];
static bool s_napt_enabled;
static int64_t s_global_session_end_time;
static bool s_heartbeat_task_started;

// --- FORWARD DECLARATIONS ---
int get_remaining_seconds(uint32_t ip);
void start_session(uint32_t ip);
uint32_t get_client_ip(httpd_req_t *req);

static esp_err_t send_redirect_to_portal(httpd_req_t *req);
static esp_err_t send_probe_success(httpd_req_t *req);
static esp_err_t ensure_napt_enabled(void);
static int get_global_remaining_seconds(void);
static client_session_t *start_or_resume_session(uint32_t ip, const uint8_t *mac, const char *token, const char *mac_hash);
static bool get_client_mac_for_ip(uint32_t ip, uint8_t mac_out[6]);
static bool get_any_connected_client_mac(uint8_t mac_out[6]);
static void generate_token_from_ip(uint32_t ip, char token_out[17]);
static void generate_hash_from_mac(const uint8_t mac[6], char mac_hash_out[65], char token_out[17]);
static void generate_hash_from_ip(uint32_t ip, char mac_hash_out[65], char token_out[17]);
static void ensure_heartbeat_task_started(void);
static void supabase_heartbeat_task(void *pvParameters);
static void refresh_portal_auth_state(const char *reason);

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

static void refresh_portal_auth_state(const char *reason)
{
    bool has_active_session = false;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            continue;
        }

        if (sessions[i].end_time > 0 && sessions[i].end_time <= now) {
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
}

static void digest_to_hex(const uint8_t digest[32], char hex_out[65], char token_out[17])
{
    if (digest == NULL || hex_out == NULL) {
        return;
    }

    for (int i = 0; i < 32; i++) {
        snprintf(&hex_out[i * 2], 3, "%02x", digest[i]);
    }
    hex_out[64] = '\0';

    if (token_out != NULL) {
        memcpy(token_out, hex_out, 16);
        token_out[16] = '\0';
    }
}

void generate_token_from_mac(const uint8_t mac[6], char token_out[17])
{
    char ignored_hash[65];

    if (mac == NULL || token_out == NULL) {
        return;
    }

    generate_hash_from_mac(mac, ignored_hash, token_out);
}

static void generate_hash_from_mac(const uint8_t mac[6], char mac_hash_out[65], char token_out[17])
{
    uint8_t digest[32];

    if (mac == NULL || mac_hash_out == NULL) {
        return;
    }

    mbedtls_sha256(mac, 6, digest, 0);
    digest_to_hex(digest, mac_hash_out, token_out);
}

static void generate_token_from_ip(uint32_t ip, char token_out[17])
{
    char ignored_hash[65];

    if (token_out == NULL) {
        return;
    }

    generate_hash_from_ip(ip, ignored_hash, token_out);
}

static void generate_hash_from_ip(uint32_t ip, char mac_hash_out[65], char token_out[17])
{
    uint8_t digest[32];
    uint8_t ip_bytes[4];

    if (mac_hash_out == NULL) {
        return;
    }

    memcpy(ip_bytes, &ip, sizeof(ip_bytes));
    mbedtls_sha256(ip_bytes, sizeof(ip_bytes), digest, 0);
    digest_to_hex(digest, mac_hash_out, token_out);
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

static client_session_t *start_or_resume_session(uint32_t ip, const uint8_t *mac, const char *token, const char *mac_hash)
{
    int64_t now = esp_timer_get_time();
    int session_index = -1;

    if (ip == 0) {
        s_global_session_end_time = now + SESSION_DURATION_US;
        refresh_portal_auth_state("portal_accept_global");
        if (token == NULL || token[0] == '\0') {
            ESP_LOGW(TAG_WEB, "Client IP lookup failed; using global session fallback");
            net_diag_log_snapshot("session_global_fallback");
            net_diag_schedule_probe("portal_accept");
            return NULL;
        }

        session_index = find_session_index_by_token(token);
    } else {
        session_index = find_session_index_by_ip(ip);
    }

    if (session_index >= 0) {
        if (sessions[session_index].end_time > now) {
            if (mac != NULL) {
                memcpy(sessions[session_index].mac, mac, 6);
            }
            if (token != NULL && token[0] != '\0') {
                strlcpy(sessions[session_index].token, token, sizeof(sessions[session_index].token));
            }
            if (mac_hash != NULL && mac_hash[0] != '\0') {
                strlcpy(sessions[session_index].mac_hash, mac_hash, sizeof(sessions[session_index].mac_hash));
            }
            refresh_portal_auth_state("session_reused");
            ESP_LOGI(TAG_WEB, "Session exists for IP %lu. Keeping existing time.", ip);
            net_diag_log_snapshot("session_reused");
            return &sessions[session_index];
        }

        sessions[session_index].active = false;
    }

    session_index = find_session_index_by_token(token);
    if (session_index >= 0) {
        sessions[session_index].ip_addr = ip;
        if (mac != NULL) {
            memcpy(sessions[session_index].mac, mac, 6);
        }
        if (mac_hash != NULL && mac_hash[0] != '\0') {
            strlcpy(sessions[session_index].mac_hash, mac_hash, sizeof(sessions[session_index].mac_hash));
        }
        sessions[session_index].end_time = now + SESSION_DURATION_US;
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

    sessions[session_index].ip_addr = ip;
    sessions[session_index].end_time = now + SESSION_DURATION_US;
    sessions[session_index].active = true;
    if (mac != NULL) {
        memcpy(sessions[session_index].mac, mac, 6);
    }
    if (token != NULL) {
        strlcpy(sessions[session_index].token, token, sizeof(sessions[session_index].token));
    }
    if (mac_hash != NULL) {
        strlcpy(sessions[session_index].mac_hash, mac_hash, sizeof(sessions[session_index].mac_hash));
    }

    if (ip != 0) {
        s_global_session_end_time = 0;
    }
    refresh_portal_auth_state("session_started");
    ESP_LOGI(TAG_WEB, "Starting session for IP: %lu token=%s", ip,
             sessions[session_index].token[0] != '\0' ? sessions[session_index].token : "(none)");
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
    if (ip != 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].active && sessions[i].ip_addr == ip) {
                int64_t remaining = sessions[i].end_time - now;
                if (remaining <= 0) {
                    sessions[i].active = false;
                    refresh_portal_auth_state("session_expired");
                    return -1;
                }
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
    start_or_resume_session(ip, NULL, NULL, NULL);
}

void handle_client_disconnect(const uint8_t mac[6])
{
    char token[17] = {0};

    if (mac == NULL) {
        return;
    }

    generate_token_from_mac(mac, token);
    ESP_LOGI(TAG_WEB, "Handling AP disconnect for token %s", token);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            continue;
        }

        if (memcmp(sessions[i].mac, mac, 6) == 0 || strcmp(sessions[i].token, token) == 0) {
            sessions[i].active = false;
        }
    }

    refresh_portal_auth_state("ap_client_disconnected");

    if (token[0] != '\0') {
        esp_err_t err = supabase_mark_disconnected(token);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_WEB, "Failed to mark token %s disconnected in Supabase: %s",
                     token, esp_err_to_name(err));
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

            remaining = get_remaining_seconds(sessions[i].ip_addr);
            if (remaining > 0) {
                esp_err_t err = supabase_update_heartbeat(sessions[i].token, remaining);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG_WEB, "Heartbeat failed for token %s: %s",
                             sessions[i].token, esp_err_to_name(err));
                }
                continue;
            }

            ESP_LOGI(TAG_WEB, "Session expired for token %s", sessions[i].token);
            supabase_update_heartbeat(sessions[i].token, 0);
            sessions[i].active = false;
            refresh_portal_auth_state("session_expired_heartbeat");
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
    snprintf(expected, sizeof(expected), "%s:%s", ADMIN_USERNAME, ADMIN_PASSWORD);

    if (strcmp((char *)decoded, expected) == 0) return AUTH_OK;

    return AUTH_WRONG;
}
// ----------------------------------------

/* Admin Config Handler */
static esp_err_t admin_config_handler(httpd_req_t *req)
{
    // --- AUTHENTICATION CHECK ---
    auth_status_t status = check_auth(req);

    // If Auth is MISSING or WRONG -> Send 401 with Redirect Body
    if (status != AUTH_OK) {
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
            
        // We MUST send 401 to make the browser prompt again
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"" AUTH_REALM "\"");
        httpd_resp_send(req, redirect_html, strlen(redirect_html));
        return ESP_OK;
    }
    // -----------------------------

    // 3. If Auth Success -> Run original code
    char* buf;
    size_t buf_len;
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (strcmp(buf, "reset=Reboot") == 0) esp_timer_start_once(restart_timer, 500000);
            char param1[64]; char param2[64]; 
            if (httpd_query_key_value(buf, "ap_ssid", param1, sizeof(param1)) == ESP_OK) {
                preprocess_string(param1);
                if (httpd_query_key_value(buf, "ap_password", param2, sizeof(param2)) == ESP_OK) {
                    preprocess_string(param2);
                    char* argv[] = {"set_ap", param1, param2};
                    set_ap(3, argv);
                    esp_timer_start_once(restart_timer, 500000);
                }
            }
            if (httpd_query_key_value(buf, "ssid", param1, sizeof(param1)) == ESP_OK) {
                preprocess_string(param1);
                if (httpd_query_key_value(buf, "password", param2, sizeof(param2)) == ESP_OK) {
                    preprocess_string(param2);
                    char* argv[] = {"set_sta", param1, param2, "-u", "", "-a", ""}; 
                    int argc = 3;
                    set_sta(argc, argv);
                    esp_timer_start_once(restart_timer, 500000);
                }
            }
        }
        free(buf);
    }
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
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
        "</style>"
        "<script>"
        " function toggleButton() {"
        "   var checkBox = document.getElementById('agree');"
        "   var btn = document.getElementById('connectBtn');"
        "   if (checkBox.checked){ btn.style.pointerEvents = 'auto'; btn.style.opacity = '1'; } "
        "   else { btn.style.pointerEvents = 'none'; btn.style.opacity = '0.5'; }"
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
        "    <a href='/confirm' id='connectBtn' class='btn' style='pointer-events: none; opacity: 0.5;'>Start Browsing</a>"
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
    char token[17] = {0};
    char mac_hash[65] = {0};
    client_session_t *session;
    const char *session_token;
    const char *session_mac_hash;
    const char *dashboard_url;
    char dashboard_url_buf[128];
    net_diag_log_snapshot("confirm_handler");

    have_mac = get_client_mac_for_ip(ip, client_mac);
    if (!have_mac && get_any_connected_client_mac(client_mac)) {
        have_mac = true;
        ESP_LOGW(TAG_WEB, "Falling back to the first connected AP client MAC for /confirm");
    }

    if (have_mac) {
        generate_hash_from_mac(client_mac, mac_hash, token);
    } else if (ip != 0) {
        ip4_addr_t ip_addr = { .addr = ip };
        generate_hash_from_ip(ip, mac_hash, token);
        ESP_LOGW(TAG_WEB, "Falling back to IP-derived token/hash for client " IPSTR, IP2STR(&ip_addr));
    } else {
        ESP_LOGW(TAG_WEB, "Unable to derive token/hash for /confirm; dashboard link will omit token");
    }

    // --- START THE INTERNET HERE ---
    session = start_or_resume_session(ip,
                                      have_mac ? client_mac : NULL,
                                      token[0] != '\0' ? token : NULL,
                                      mac_hash[0] != '\0' ? mac_hash : NULL);

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

    session_token = (session != NULL && session->token[0] != '\0') ? session->token : (token[0] != '\0' ? token : NULL);
    session_mac_hash = (session != NULL && session->mac_hash[0] != '\0') ? session->mac_hash : (mac_hash[0] != '\0' ? mac_hash : NULL);

    if (session_token != NULL && session_mac_hash != NULL) {
        ESP_LOGI(TAG_WEB, "Syncing portal session to Supabase with token %s", session_token);
        esp_err_t sync_err = supabase_create_session(session_token,
                                                     session_mac_hash,
                                                     remaining_seconds > 0 ? remaining_seconds : SESSION_DURATION_SEC);
        if (sync_err == ESP_ERR_INVALID_STATE) {
            sync_err = supabase_update_heartbeat(session_token, remaining_seconds);
        }

        if (sync_err != ESP_OK) {
            ESP_LOGW(TAG_WEB, "Failed to sync session %s to Supabase: %s",
                     session_token, esp_err_to_name(sync_err));
        }
    } else {
        ESP_LOGW(TAG_WEB, "Skipping Supabase sync because token or mac_hash is missing");
    }

    if (session != NULL && session->token[0] != '\0') {
        snprintf(dashboard_url_buf, sizeof(dashboard_url_buf),
                 "https://spcs-v1.vercel.app/dashboard?token=%s", session->token);
        dashboard_url = dashboard_url_buf;
    } else if (token[0] != '\0') {
        snprintf(dashboard_url_buf, sizeof(dashboard_url_buf),
                 "https://spcs-v1.vercel.app/dashboard?token=%s", token);
        dashboard_url = dashboard_url_buf;
    } else {
        dashboard_url = "https://spcs-v1.vercel.app/dashboard";
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
    char* safe_ap_ssid = html_escape(ap_ssid); char* safe_ap_passwd = html_escape(ap_passwd);
    char* safe_ssid = html_escape(ssid); char* safe_passwd = html_escape(passwd);
    char* safe_ent_username = html_escape(ent_username); char* safe_ent_identity = html_escape(ent_identity);
    int page_len = strlen(config_page_template) + strlen(safe_ap_ssid) + strlen(safe_ap_passwd) + 
                   strlen(safe_ssid) + strlen(safe_passwd) + strlen(safe_ent_username) + 
                   strlen(safe_ent_identity) + 256;
    char* config_page = malloc(sizeof(char) * page_len);
    snprintf(config_page, page_len, config_page_template,
        safe_ap_ssid, safe_ap_passwd, safe_ssid, safe_passwd, safe_ent_username, safe_ent_identity,
        static_ip, subnet_mask, gateway_addr);
    free(safe_ap_ssid); free(safe_ap_passwd); free(safe_ssid);
    free(safe_passwd); free(safe_ent_username); free(safe_ent_identity);

    if (httpd_start(&server, &config) != ESP_OK) return NULL;

    httpd_uri_t api_status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_status_uri);
    httpd_uri_t portal_uri = { .uri = "/", .method = HTTP_GET, .handler = portal_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &portal_uri);
    
    httpd_uri_t confirm_uri = { .uri = "/confirm", .method = HTTP_GET, .handler = confirm_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &confirm_uri);
    httpd_uri_t admin_uri = { .uri = "/config", .method = HTTP_GET, .handler = admin_config_handler, .user_ctx = config_page };
    httpd_register_uri_handler(server, &admin_uri);
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
