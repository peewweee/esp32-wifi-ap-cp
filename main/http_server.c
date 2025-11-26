/*
 * Captive Portal HTTP Server
 *
 * Flow:
 * 1. User Connects -> Portal Page (/)
 * 2. User Clicks Start -> Redirects to /confirm
 * 3. /confirm -> Enables Internet, Starts Timer, Displays Link to Root PWA
 */
#include <stdio.h>
#include <sys/stat.h>
#include "esp_spiffs.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include "esp_netif.h"
#include <esp_http_server.h>
    
// Required for NAT control
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h" 

// Your project's global variables
#include "router_globals.h"

#include "pages.h"
#include "esp_timer.h"

static const char *TAG_WEB = "CAPTIVE_PORTAL";
static const char *TAG_ADMIN = "ADMIN_SERVER";

// --- SESSION CONFIGURATION ---
#define MAX_CLIENTS 20
// TIMER SET TO 60 SECONDS FOR TESTING
#define SESSION_DURATION_SEC 60 
#define SESSION_DURATION_US (SESSION_DURATION_SEC * 1000000LL)

// --- PER-DEVICE SESSION STRUCTURE ---
typedef struct {
    uint32_t ip_addr;       // The client's IP address
    int64_t end_time;       // When their session expires (internal ESP time)
    bool active;            // Is this slot in use?
} client_session_t;

static client_session_t sessions[MAX_CLIENTS];

// --- FORWARD DECLARATIONS ---
int get_remaining_seconds(uint32_t ip);
void start_session(uint32_t ip);
uint32_t get_client_ip(httpd_req_t *req);

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
    for(int i=0; i<MAX_CLIENTS; i++) sessions[i].active = false;
}

uint32_t get_client_ip(httpd_req_t *req) {
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
        return addr.sin_addr.s_addr;
    }
    return 0;
}

int get_remaining_seconds(uint32_t ip) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && sessions[i].ip_addr == ip) {
            int64_t remaining = sessions[i].end_time - now;
            if (remaining <= 0) {
                sessions[i].active = false; 
                return -1;
            }
            return (int)(remaining / 1000000LL);
        }
    }
    return -1; 
}

void start_session(uint32_t ip) {
    int64_t now = esp_timer_get_time();
    
    // Check if user already exists
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && sessions[i].ip_addr == ip) {
            // If session exists and has time left, DO NOT reset it.
            if (sessions[i].end_time > now) {
                ESP_LOGI(TAG_WEB, "Session exists for IP %lu. Keeping existing time.", ip);
                return; 
            }
            break;
        }
    }
    
    // Create new session
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            sessions[i].ip_addr = ip;
            sessions[i].end_time = now + SESSION_DURATION_US;
            sessions[i].active = true;
            ESP_LOGI(TAG_WEB, "Starting NEW session for IP: %lu", ip);
            return;
        }
    }
    
    // Fallback: overwrite first slot
    sessions[0].ip_addr = ip;
    sessions[0].end_time = now + SESSION_DURATION_US;
    sessions[0].active = true;
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

/* Admin Config Handler */
static esp_err_t admin_config_handler(httpd_req_t *req)
{
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
        "   <h1>&lsquo;CPE Wi-Fi&rsquo;</h1>"
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
static esp_err_t redirect_handler(httpd_req_t *req)
{
    uint32_t ip = get_client_ip(req);
    int remaining = get_remaining_seconds(ip);

    if (remaining > 0) {
        // Authenticated. Return 404 to stop Captive Portal popup from hanging around.
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // Not authenticated. Redirect to Portal IP.
    httpd_resp_set_status(req, "302 Found");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(wifiAP, &ip_info);
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    
    char redirect_url[64];
    snprintf(redirect_url, sizeof(redirect_url), "http://%s", ip_str);
    httpd_resp_set_hdr(req, "Location", redirect_url);
    httpd_resp_send(req, NULL, 0);

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
    
    // --- START THE INTERNET HERE ---
    start_session(ip); // Starts session or keeps existing one
    
    ip_napt_enable(my_ap_ip, 1);
    
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
    }
    // -------------------------------

    int remaining_seconds = get_remaining_seconds(ip);
    if (remaining_seconds < 0) remaining_seconds = 0;

    ESP_LOGI(TAG_WEB, "Confirm Page. IP: %lu, Remaining: %d", ip, remaining_seconds);

    char *resp_str = malloc(4096); 
    if (resp_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(resp_str, 4096,
    "<!DOCTYPE html><html><head><title>Connected to CPE Wi-Fi</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<style>"
    "body { font-family: Inter, sans-serif; text-align: center; background-color: #F5EFE6; margin: 0; padding: 0; color: #4A3B32; }"
    ".container { max-width: 600px; margin: 0 auto; padding: 0 20px 40px 20px; }"
    /* --- UPDATED HERO CLASS --- */
    /* height: 50vh means 50% of viewport height. object-fit: cover prevents squishing. */
    ".hero { width: 100%%; height: 50vh; object-fit: cover; display: block; margin-bottom: 10px; mask-image: linear-gradient(to bottom, black 60%%, transparent 100%%); -webkit-mask-image: linear-gradient(to bottom, black 60%%, transparent 100%%); }"
    /* -------------------------- */
    "h1 { color: #6F1D1B; font-size: 24px; font-weight: 400; margin: 0; }"
    "h1 b { font-weight: 700; display: block; }"
    "hr { border: 0; height: 1px; background-color: #E0D8D0; margin: 30px 0; }"
    ".dash-area { text-align: left; }"
    "h2 { font-size: 18px; font-weight: 700; margin-bottom: 10px; }"
    ".btn { display: inline-block; background-color: #1A1A1A; color: white; text-decoration: none; padding: 10px 15px; border-radius: 8px; font-weight: 600; margin-bottom: 20px; }"
    ".grid { display: flex; gap: 10px; justify-content: center; align-items: flex-start; }"
    ".grid img { max-width: 100%%; border-radius: 12px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }"
    ".p-img { width: 45%%; }"
    ".d-img { width: 50%%; margin-top: 40px; }"
    "</style>"
    "</head><body>"
    "<img src='/cea.png' class='hero' />"
    "<div class='container'>"
    "  <h1>You are now connected to<br><b>'CPE Wi-Fi'</b></h1>"
    "  <hr>"
    "  <div class='dash-area'>"
    "    <h2>Explore the dashboard</h2>"
    "    <a href='https://spcs-v1.vercel.app?connected=true&seconds=%d' target='_blank' class='btn'>Solar-Powered Charging Station</a>"
    "    <div class='grid'>"
    "      <img src='/dashboard.png'  class='p-img'/>"
    "      <img src='/dashboard-ui.png' class='d-img'/>"
    "    </div>"
    "  </div>"
    "</div>"
    "</body></html>",
    remaining_seconds // This integer replaces the %d in the URL above
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
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.stack_size = 8192;
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
    httpd_uri_t connectivity_check_uri = { .uri = "/connectivity-check.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &connectivity_check_uri);
    httpd_uri_t hotspot_uri = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &hotspot_uri);
    httpd_uri_t cea_uri = { .uri = "/cea.png", .method = HTTP_GET, .handler = img_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &cea_uri);
    httpd_uri_t phone_uri = { .uri = "/dashboard.png", .method = HTTP_GET, .handler = img_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &phone_uri);
    httpd_uri_t dash_uri = { .uri = "/dashboard-ui.png", .method = HTTP_GET, .handler = img_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &dash_uri);
    httpd_uri_t catch_all_uri = { .uri = "/*", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &catch_all_uri);

    return server;
}