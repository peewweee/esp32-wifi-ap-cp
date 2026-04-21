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

static const char *supabase_auth_mode_name(void)
{
    return uses_legacy_jwt_key() ? "legacy Bearer JWT" : "sb_* secret key";
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

static const char *http_method_name(esp_http_client_method_t method)
{
    switch (method) {
        case HTTP_METHOD_POST:
            return "POST";
        case HTTP_METHOD_PATCH:
            return "PATCH";
        default:
            return "HTTP";
    }
}

static esp_err_t perform_request(esp_http_client_method_t method,
                                 const char *path,
                                 const char *body,
                                 const char *prefer_header)
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
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    if (prefer_header != NULL && prefer_header[0] != '\0') {
        esp_http_client_set_header(client, "Prefer", prefer_header);
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
             http_method_name(method),
             path,
             status_code);
    if (response_len > 0) {
        ESP_LOGI(TAG, "Supabase response body: %s", response_buf);
    }
    esp_http_client_cleanup(client);

    if (status_code >= 200 && status_code < 300) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Supabase request failed for %s with HTTP %d", path, status_code);

    return ESP_FAIL;
}

void supabase_init(void)
{
    if (s_initialized) {
        return;
    }

    s_initialized = true;

    if (has_placeholder_key()) {
        ESP_LOGW(TAG, "Supabase client initialized without a real API key; writes will fail until SUPABASE_API_KEY is set at build time");
        return;
    }

    ESP_LOGI(TAG, "Supabase client initialized for %s using %s",
             SUPABASE_BASE_URL,
             supabase_auth_mode_name());
}

static esp_err_t supabase_upsert_session(const char *session_token,
                                         const char *device_hash,
                                         int remaining_sec,
                                         bool ap_connected,
                                         const char *status)
{
    char body[768];
    char session_end[32];
    char heartbeat[32];
    int safe_remaining = remaining_sec > 0 ? remaining_sec : 0;
    time_t now;
    bool have_time;

    if (session_token == NULL || session_token[0] == '\0' ||
        device_hash == NULL || device_hash[0] == '\0' ||
        status == NULL || status[0] == '\0') {
        ESP_LOGE(TAG, "Refusing Supabase upsert because session_token/device_hash/status is missing");
        return ESP_ERR_INVALID_ARG;
    }

    have_time = get_valid_time(&now);
    if (have_time) {
        time_t end_time = now;
        if (safe_remaining > 0) {
            end_time += safe_remaining;
        }

        format_timestamp_utc(end_time, session_end, sizeof(session_end));
        format_timestamp_utc(now, heartbeat, sizeof(heartbeat));
        snprintf(body, sizeof(body),
                 "{\"token\":\"%s\",\"mac_hash\":\"%s\","
                 "\"session_token\":\"%s\",\"device_hash\":\"%s\",\"session_end\":\"%s\","
                 "\"remaining_seconds\":%d,\"status\":\"%s\",\"ap_connected\":%s,"
                 "\"last_heartbeat\":\"%s\"}",
                 session_token,
                 device_hash,
                 session_token,
                 device_hash,
                 session_end,
                 safe_remaining,
                 status,
                 ap_connected ? "true" : "false",
                 heartbeat);
    } else {
        ESP_LOGW(TAG, "System clock is not set; session upsert will omit timestamps");
        snprintf(body, sizeof(body),
                 "{\"token\":\"%s\",\"mac_hash\":\"%s\","
                 "\"session_token\":\"%s\",\"device_hash\":\"%s\",\"remaining_seconds\":%d,"
                 "\"status\":\"%s\",\"ap_connected\":%s}",
                 session_token,
                 device_hash,
                 session_token,
                 device_hash,
                 safe_remaining,
                 status,
                 ap_connected ? "true" : "false");
    }

    ESP_LOGI(TAG, "Upserting session to Supabase: token=%s device_hash=%s status=%s remaining=%d ap_connected=%s",
             session_token,
             device_hash,
             status,
             safe_remaining,
             ap_connected ? "true" : "false");
    ESP_LOGI(TAG, "Supabase target table: public.sessions");

    return perform_request(HTTP_METHOD_POST,
                           "/rest/v1/sessions?on_conflict=token",
                           body,
                           "resolution=merge-duplicates,return=representation");
}

esp_err_t supabase_create_session(const char *session_token, const char *device_hash, int duration_sec)
{
    return supabase_upsert_session(session_token, device_hash, duration_sec, true, "active");
}

esp_err_t supabase_update_heartbeat(const char *session_token, const char *device_hash, int remaining_sec)
{
    return supabase_upsert_session(session_token,
                                   device_hash,
                                   remaining_sec,
                                   true,
                                   remaining_sec > 0 ? "active" : "expired");
}

esp_err_t supabase_mark_disconnected(const char *session_token, const char *device_hash, int remaining_sec)
{
    return supabase_upsert_session(session_token, device_hash, remaining_sec, false, "disconnected");
}
