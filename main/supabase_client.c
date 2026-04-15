#include "supabase_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "SUPABASE";

#ifndef SUPABASE_BASE_URL
#define SUPABASE_BASE_URL "https://pzkatqdyxexkevyccjzm.supabase.co"
#endif

#ifndef SUPABASE_API_KEY
#define SUPABASE_API_KEY "REPLACE_WITH_SUPABASE_SECRET_KEY"
#endif

#define SUPABASE_TIMEOUT_MS 5000

static bool s_initialized;

static bool has_placeholder_key(void)
{
    return strcmp(SUPABASE_API_KEY, "REPLACE_WITH_SUPABASE_SECRET_KEY") == 0;
}

static bool uses_legacy_jwt_key(void)
{
    return strncmp(SUPABASE_API_KEY, "sb_", 3) != 0;
}

static bool format_timestamp_utc(time_t timestamp, char *buf, size_t buf_len)
{
    struct tm tm_utc = {0};

    if (buf == NULL || buf_len < 21) {
        return false;
    }

    if (gmtime_r(&timestamp, &tm_utc) == NULL) {
        return false;
    }

    return strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0;
}

static bool get_valid_time(time_t *out_now)
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

static esp_err_t perform_request(esp_http_client_method_t method,
                                 const char *path,
                                 const char *body,
                                 bool return_representation)
{
    char url[256];
    char auth_header[160];
    char response_buf[512];
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status_code;
    int response_len;

    if (has_placeholder_key()) {
        ESP_LOGE(TAG, "Supabase API key is still the placeholder text");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(url, sizeof(url), "%s%s", SUPABASE_BASE_URL, path);

    config.url = url;
    config.method = method;
    config.timeout_ms = SUPABASE_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.user_agent = "esp32-firmware/1.0";

    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client for %s", path);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "apikey", SUPABASE_API_KEY);
    if (uses_legacy_jwt_key()) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_API_KEY);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    if (return_representation) {
        esp_http_client_set_header(client, "Prefer", "return=representation");
    }

    if (body != NULL) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request to %s failed: %s", path, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    status_code = esp_http_client_get_status_code(client);
    response_len = esp_http_client_read_response(client, response_buf, sizeof(response_buf) - 1);
    if (response_len < 0) {
        response_len = 0;
    }
    response_buf[response_len] = '\0';

    ESP_LOGI(TAG, "%s %s -> HTTP %d",
             method == HTTP_METHOD_POST ? "POST" : "PATCH",
             path,
             status_code);
    esp_http_client_cleanup(client);

    if (status_code >= 200 && status_code < 300) {
        return ESP_OK;
    }

    if (status_code == 409) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_FAIL;
}

void supabase_init(void)
{
    if (s_initialized) {
        return;
    }

    s_initialized = true;

    if (has_placeholder_key()) {
        ESP_LOGW(TAG, "Supabase client initialized without a real API key");
        return;
    }

    ESP_LOGI(TAG, "Supabase client initialized using %s auth mode",
             uses_legacy_jwt_key() ? "legacy Bearer JWT" : "apikey-only secret key");
}

esp_err_t supabase_create_session(const char *token, const char *mac_hash, int duration_sec)
{
    char body[512];
    char session_end[32];
    char heartbeat[32];
    time_t now;
    bool have_time;

    if (token == NULL || token[0] == '\0' || mac_hash == NULL || mac_hash[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    have_time = get_valid_time(&now);
    if (!have_time) {
        ESP_LOGW(TAG, "System clock is not set; using placeholder session_end");
        snprintf(body, sizeof(body),
                 "{\"token\":\"%s\",\"mac_hash\":\"%s\",\"session_end\":\"2099-12-31T23:59:59Z\","
                 "\"remaining_seconds\":%d,\"status\":\"active\",\"ap_connected\":true}",
                 token, mac_hash, duration_sec);
    } else {
        format_timestamp_utc(now + duration_sec, session_end, sizeof(session_end));
        format_timestamp_utc(now, heartbeat, sizeof(heartbeat));
        snprintf(body, sizeof(body),
                 "{\"token\":\"%s\",\"mac_hash\":\"%s\",\"session_end\":\"%s\",\"remaining_seconds\":%d,"
                 "\"status\":\"active\",\"ap_connected\":true,\"last_heartbeat\":\"%s\"}",
                 token, mac_hash, session_end, duration_sec, heartbeat);
    }

    return perform_request(HTTP_METHOD_POST, "/rest/v1/sessions", body, true);
}

esp_err_t supabase_update_heartbeat(const char *token, int remaining_sec)
{
    char path[160];
    char body[256];
    char heartbeat[32];
    time_t now;
    bool have_time;
    bool expired = remaining_sec <= 0;
    int safe_remaining = remaining_sec > 0 ? remaining_sec : 0;

    if (token == NULL || token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(path, sizeof(path), "/rest/v1/sessions?token=eq.%s", token);

    have_time = get_valid_time(&now);
    if (have_time) {
        format_timestamp_utc(now, heartbeat, sizeof(heartbeat));
        snprintf(body, sizeof(body),
                 expired
                     ? "{\"remaining_seconds\":%d,\"last_heartbeat\":\"%s\",\"status\":\"expired\",\"ap_connected\":true}"
                     : "{\"remaining_seconds\":%d,\"last_heartbeat\":\"%s\",\"ap_connected\":true}",
                 safe_remaining, heartbeat);
    } else {
        ESP_LOGW(TAG, "System clock is not set; heartbeat will omit last_heartbeat");
        snprintf(body, sizeof(body),
                 expired
                     ? "{\"remaining_seconds\":%d,\"status\":\"expired\",\"ap_connected\":true}"
                     : "{\"remaining_seconds\":%d,\"ap_connected\":true}",
                 safe_remaining);
    }

    return perform_request(HTTP_METHOD_PATCH, path, body, false);
}

esp_err_t supabase_mark_disconnected(const char *token)
{
    char path[160];

    if (token == NULL || token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(path, sizeof(path), "/rest/v1/sessions?token=eq.%s", token);
    return perform_request(HTTP_METHOD_PATCH,
                           path,
                           "{\"ap_connected\":false,\"status\":\"disconnected\"}",
                           false);
}
