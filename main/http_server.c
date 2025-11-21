/*
 * Captive Portal HTTP Server
 *
 * This code intercepts all web traffic from new clients and serves a
 * "login" page. When the user clicks "Accept", it enables NAT
 * and fixes the DNS, granting internet access.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include "esp_netif.h"
#include <esp_http_server.h>
    
// Required for NAT control
#include "lwip/lwip_napt.h"

// Your project's global variables
#include "router_globals.h"

#include "pages.h"
#include "esp_timer.h"

static const char *TAG_WEB = "CAPTIVE_PORTAL";
static const char *TAG_ADMIN = "ADMIN_SERVER";

// --- SESSION VARIABLES ---
// FIX 1: Added LL to ensure 64-bit calculation
static const int64_t SESSION_DURATION_US = 1 * 60 * 1000000LL; 
static int64_t session_end_time = 0;

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

char* html_escape(const char* src) {
    //Primitive html attribue escape, should handle most common issues.
    int len = strlen(src);
    //Every char in the string + a null
    int esc_len = len + 1;

    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' || src[i] == '\'' || src[i] == '\"' || src[i] == '&' || src[i] == '#' || src[i] == ';') {
            //Will be replaced with a 5 char sequence
            esc_len += 4;
        }
    }

    char* res = malloc(sizeof(char) * esc_len);

    int j = 0;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' || src[i] == '\'' || src[i] == '\"' || src[i] == '&' || src[i] == '#' || src[i] == ';') {
            res[j++] = '&';
            res[j++] = '#';
            res[j++] = '0' + (src[i] / 10);
            res[j++] = '0' + (src[i] % 10);
            res[j++] = ';';
        }
        else {
            res[j++] = src[i];
        }
    }
    res[j] = '\0';

    return res;
}

/* An HTTP GET handler for the Admin Config page */
static esp_err_t admin_config_handler(httpd_req_t *req)
{
    char* buf;
    size_t buf_len;

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG_ADMIN, "Found URL query => %s", buf);
            if (strcmp(buf, "reset=Reboot") == 0) {
                esp_timer_start_once(restart_timer, 500000);
            }
            char param1[64];
            char param2[64];
            char param3[64];
            char param4[64];
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "ap_ssid", param1, sizeof(param1)) == ESP_OK) {
                ESP_LOGI(TAG_ADMIN, "Found URL query parameter => ap_ssid=%s", param1);
                preprocess_string(param1);
                if (httpd_query_key_value(buf, "ap_password", param2, sizeof(param2)) == ESP_OK) {
                    ESP_LOGI(TAG_ADMIN, "Found URL query parameter => ap_password=%s", param2);
                    preprocess_string(param2);
                    int argc = 3;
                    char* argv[3];
                    argv[0] = "set_ap";
                    argv[1] = param1;
                    argv[2] = param2;
                    set_ap(argc, argv);
                    esp_timer_start_once(restart_timer, 500000);
                }
            }
            if (httpd_query_key_value(buf, "ssid", param1, sizeof(param1)) == ESP_OK) {
                ESP_LOGI(TAG_ADMIN, "Found URL query parameter => ssid=%s", param1);
                preprocess_string(param1);
                if (httpd_query_key_value(buf, "password", param2, sizeof(param2)) == ESP_OK) {
                    ESP_LOGI(TAG_ADMIN, "Found URL query parameter => password=%s", param2);
                    preprocess_string(param2);
                    if (httpd_query_key_value(buf, "ent_username", param3, sizeof(param3)) == ESP_OK) {
                        ESP_LOGI(TAG_ADMIN, "Found URL query parameter => ent_username=%s", param3);
                        preprocess_string(param3);
                        if (httpd_query_key_value(buf, "ent_identity", param4, sizeof(param4)) == ESP_OK) {
                            ESP_LOGI(TAG_ADMIN, "Found URL query parameter => ent_identity=%s", param4);
                            preprocess_string(param4);
                            int argc = 0;
                            char* argv[7];
                            argv[argc++] = "set_sta";
                            argv[argc++] = param1; //SSID
                            argv[argc++] = param2; //Password
                            if(strlen(param2)) { //Username
                                argv[argc++] = "-u";
                                argv[argc++] = param3;
                            }
                            if(strlen(param3)) { //Identity
                                argv[argc++] = "-a";
                                argv[argc++] = param4;
                            }
                            set_sta(argc, argv);
                            esp_timer_start_once(restart_timer, 500000);
                        }
                    }
                }
            }
            if (httpd_query_key_value(buf, "staticip", param1, sizeof(param1)) == ESP_OK) {
                ESP_LOGI(TAG_ADMIN, "Found URL query parameter => staticip=%s", param1);
                preprocess_string(param1);
                if (httpd_query_key_value(buf, "subnetmask", param2, sizeof(param2)) == ESP_OK) {
                    ESP_LOGI(TAG_ADMIN, "Found URL query parameter => subnetmask=%s", param2);
                    preprocess_string(param2);
                    if (httpd_query_key_value(buf, "gateway", param3, sizeof(param3)) == ESP_OK) {
                        ESP_LOGI(TAG_ADMIN, "Found URL query parameter => gateway=%s", param3);
                        preprocess_string(param3);
                        int argc = 4;
                        char* argv[4];
                        argv[0] = "set_sta_static";
                        argv[1] = param1;
                        argv[2] = param2;
                        argv[3] = param3;
                        set_sta_static(argc, argv);
                        esp_timer_start_once(restart_timer, 500000);
                    }
                }
            }
        }
        free(buf);
    }

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, strlen(resp_str));

    return ESP_OK;
}


// --- HELPER: Check if session is valid ---
// Returns true if connected, false if expired or not started
bool check_session_validity() {
    int64_t current_time = esp_timer_get_time();

    // Case 1: Session never started
    if (session_end_time == 0) {
        return false;
    }

    // Case 2: Session Expired
    if (current_time > session_end_time) {
        ESP_LOGI(TAG_WEB, "Session time limit reached. Disconnecting user.");
        
        // Disable NAT immediately
        ip_napt_enable(my_ap_ip, 0);
        
        // Reset session
        session_end_time = 0;
        portal_authenticated = false; 
        return false;
    }

    // Case 3: Session Valid
    return true;
}

/*
 * This handler is called when the user clicks "Accept".
 * It enables NAT and fixes the DNS for all clients.
 */
static esp_err_t accept_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WEB, "User accepted terms. Starting Session Timer.");
    
    // 1. Set the Session End Time
    session_end_time = esp_timer_get_time() + SESSION_DURATION_US;
    portal_authenticated = true;

    // 2. ENABLE NAT
    ip_napt_enable(my_ap_ip, 1);

    // 3. FIX DNS (Push real DNS to clients)
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
    }

    // 4. Redirect back to the root page (which will now show the status/timer)
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

/*
 * This handler serves the main "login" page.
 */
static esp_err_t portal_handler(httpd_req_t *req)
{
    // Check if time remains
    bool is_connected = check_session_validity();

    if (is_connected) {
        // --- SHOW STATUS PAGE WITH TIMER ---
        
        int64_t remaining_us = session_end_time - esp_timer_get_time();
        int remaining_seconds = (int)(remaining_us / 1000000);
        int minutes = remaining_seconds / 60;
        int seconds = remaining_seconds % 60;

        // FIX 2: Increased buffer from 1024 to 2048 to prevent truncation error
        char resp_buf[2048];
        
        snprintf(resp_buf, sizeof(resp_buf), 
            "<html><head><title>Status</title>"
            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
            "<meta http-equiv='refresh' content='60'>" // Refresh page every 60s to update server time
            "<style>"
            "body { font-family: Arial, sans-serif; background-color: #e8f5e9; text-align: center; padding: 50px; }"
            ".timer-box { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); display: inline-block; }"
            "h1 { color: #2e7d32; }"
            ".time { font-size: 48px; font-weight: bold; color: #333; }"
            ".label { font-size: 14px; color: #666; }"
            "</style>"
            "<script>"
            "  // Simple JS countdown for visual smoothness between refreshes\n"
            "  var secondsLeft = %d;\n"
            "  function countdown() {\n"
            "    var m = Math.floor(secondsLeft / 60);\n"
            "    var s = secondsLeft %% 60;\n" // Note: double % is escape for printf
            "    if(s < 10) s = '0' + s;\n"
            "    document.getElementById('timer').innerHTML = m + ':' + s;\n"
            "    if (secondsLeft > 0) { secondsLeft--; setTimeout(countdown, 1000); }\n"
            "    else { location.reload(); }\n" // Reload when time is up to trigger disconnect
            "  }\n"
            "  window.onload = countdown;\n"
            "</script>"
            "</head>"
            "<body>"
            "  <div class='timer-box'>"
            "    <h1>Connected</h1>"
            "    <p>You have internet access.</p>"
            "    <div class='time' id='timer'>%d:%02d</div>"
            "    <div class='label'>Time Remaining</div>"
            "  </div>"
            "</body></html>",
            remaining_seconds, minutes, seconds
        );

        httpd_resp_send(req, resp_buf, strlen(resp_buf));
        return ESP_OK;
    }
    
    // --- SHOW LOGIN PAGE (If not connected) ---
    const char* resp_str = 
        "<html><head><title>CPE Wi-Fi</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<style>"
        " body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: #F5F0EB; color: #521B1B; display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; margin: 0; padding: 20px; box-sizing: border-box; }"
        " .icon { width: 80px; height: 80px; fill: #521B1B; margin-bottom: 20px; }"
        " h2 { font-size: 20px; font-weight: normal; color: #6D3B39; margin: 0; text-align: center; }"
        " h1 { font-size: 28px; font-weight: bold; margin: 5px 0 30px 0; text-align: center; }"
        " hr { width: 100%%; max-width: 400px; text-align: left; }"
        " h3 { font-size: 16px; font-weight: bold; margin-bottom: 10px; color: #521B1B; }"
        " p { font-size: 12px; line-height: 1.5; color: #6D3B39; margin-bottom: 20px; text-align: justify; }"
        " .checkbox-container { display: flex; align-items: center; font-size: 13px; font-weight: bold; margin-bottom: 30px; color: #521B1B; }"
        " input[type='checkbox'] { transform: scale(1.3); margin-right: 10px; accent-color: #521B1B; cursor: pointer; }"
        " .btn { display: block; width: 100%%; max-width: 200px; padding: 12px 0; font-size: 18px; color: #fff; background-color: #1D734B; border: none; border-radius: 8px; text-decoration: none; text-align: center; margin: 0 auto; cursor: pointer; transition: background 0.3s; }"
        " .btn:hover { background-color: #145235; }"
        "</style>"
        "<script>"
        " function toggleButton() {"
        "   var checkBox = document.getElementById('agree');"
        "   var btn = document.getElementById('connectBtn');"
        "   if (checkBox.checked){ btn.style.pointerEvents = 'auto'; btn.style.opacity = '1'; } "
        "   else { btn.style.pointerEvents = 'none'; btn.style.opacity = '0.6'; }"
        " }"
        "</script>"
        "</head>"
        "<body>"
        " <svg class='icon' viewBox='0 0 24 24'>"
        "  <path d='M12 21L12 21C11.05 21 10.24 20.43 9.85 19.63L12 17L14.15 19.63C13.76 20.43 12.95 21 12 21ZM12 3C7.95 3 4.21 4.34 1.2 6.6L3 9C5.5 7.12 8.62 6 12 6C15.38 6 18.5 7.12 21 9L22.8 6.6C19.79 4.34 16.05 3 12 3ZM12 9C9.3 9 6.81 9.89 4.8 11.4L6.6 13.8C8.1 12.67 9.97 12 12 12C14.03 12 15.9 12.67 17.4 13.8L19.2 11.4C17.19 9.89 14.7 9 12 9Z'/>"
        " </svg>"
        " <h2>You are connecting to</h2>"
        " <h1>&lsquo;CPE Wi-Fi&rsquo;</h1>"
        " <div class='content'>"
        "  <hr>"
        "  <h3>Terms and Conditions</h3>"
        "  <p>By connecting to this Wi-Fi network, you agree to use the service solely for educational and academic purposes. Each device is allowed a maximum connection time of <strong>1 hour per day</strong> to ensure fair use among all users. Your usage will be logged for monitoring and maintenance purposes.</p>"
        "  <div class='checkbox-container'>"
        "   <input type='checkbox' id='agree' onclick='toggleButton()'>"
        "   <label for='agree'>I agree to the Terms and Conditions.</label>"
        "  </div>"
        "  <a href='http://localhost:3000' id='connectBtn' class='btn' style='pointer-events: none; opacity: 0.6;'>Start</a>"
        " </div>"
        "</body></html>";

    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

/*
 * This is the CATCH-ALL handler.
 * Any request to any domain (e.g., google.com, cnn.com) will be caught
 * by our DNS hijack and sent to this handler.
 * We redirect them to the main portal page.
 */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    // CRITICAL: Check if the time ran out while they were browsing
    bool is_connected = check_session_validity();

    if (is_connected) {
        // If they are connected, we usually want to return 404 for random requests
        // so their browser stops trying to hijack.
        // OR, we can redirect them to the status page if they type a random URL.
        ESP_LOGW(TAG_WEB, "Already authed. Sending 404 for %s", req->uri);
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    ESP_LOGI(TAG_WEB, "Redirecting request for '%s' to portal", req->uri);

    httpd_resp_set_status(req, "302 Found");
    
    // Get the AP's IP to redirect correctly
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

// This is the function called from main.c
httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15; // We need more handlers now
    config.stack_size = 8192;     // Increase stack size

    // Create the restart timer (for the admin page)
    esp_timer_create(&restart_timer_args, &restart_timer);

    // --- Build the Admin Config Page HTML ---
    // This is the logic from your old http_server.c
    const char* config_page_template = CONFIG_PAGE;
    char* safe_ap_ssid = html_escape(ap_ssid);
    char* safe_ap_passwd = html_escape(ap_passwd);
    char* safe_ssid = html_escape(ssid);
    char* safe_passwd = html_escape(passwd);
    char* safe_ent_username = html_escape(ent_username);
    char* safe_ent_identity = html_escape(ent_identity);

    int page_len =
        strlen(config_page_template) +
        strlen(safe_ap_ssid) +
        strlen(safe_ap_passwd) +
        strlen(safe_ssid) +
        strlen(safe_passwd) +
        strlen(safe_ent_username) +
        strlen(safe_ent_identity) +
        256;
    char* config_page = malloc(sizeof(char) * page_len);

    snprintf(
        config_page, page_len, config_page_template,
        safe_ap_ssid, safe_ap_passwd,
        safe_ssid, safe_passwd, safe_ent_username, safe_ent_identity,
            static_ip, subnet_mask, gateway_addr);
    
    free(safe_ap_ssid);
    free(safe_ap_passwd);
    free(safe_ssid);
    free(safe_passwd);
    free(safe_ent_username);
    free(safe_ent_identity);
    // --- End of Admin Page Build ---


    ESP_LOGI(TAG_WEB, "Starting web server for portal and config");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG_WEB, "Failed to start web server");
        return NULL;
    }

    // --- Register ALL Specific Handlers FIRST ---

    // Handler for the GUEST PORTAL main page
    httpd_uri_t portal_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &portal_uri);

    // Handler for the GUEST "Accept" button
    httpd_uri_t accept_uri = {
        .uri       = "/accept",
        .method    = HTTP_GET,
        .handler   = accept_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &accept_uri);

    // Handler for the ADMIN CONFIG page
    httpd_uri_t admin_uri = {
        .uri       = "/config",
        .method    = HTTP_GET,
        .handler   = admin_config_handler,
        .user_ctx  = config_page // Pass the generated HTML to the handler
    };
    httpd_register_uri_handler(server, &admin_uri);


    // --- Special Handlers for OS connectivity checks ---
    httpd_uri_t gen_204_uri = {
        .uri       = "/generate_204",
        .method    = HTTP_GET,
        .handler   = redirect_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &gen_204_uri);

    httpd_uri_t connectivity_check_uri = {
        .uri       = "/connectivity-check.html",
        .method    = HTTP_GET,
        .handler   = redirect_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &connectivity_check_uri);

    httpd_uri_t hotspot_uri = {
        .uri       = "/hotspot-detect.html",
        .method    = HTTP_GET,
        .handler   = redirect_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &hotspot_uri);
    
    // --- CATCH-ALL Handler ---
    // This MUST be registered last
    httpd_uri_t catch_all_uri = {
        .uri       = "/*", // Wildcard
        .method    = HTTP_GET,
        .handler   = redirect_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &catch_all_uri);

    ESP_LOGI(TAG_WEB, "Web server started with ALL handlers.");
    return server;
}