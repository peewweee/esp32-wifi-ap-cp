#include "admin_ports.h"
#include "supabase_client.h"

#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "admin_ports";

#define STATION_ID "solar-hub-01"

/* ------------------------------------------------------------------------
 * HTML test page
 * ------------------------------------------------------------------------ */
static const char *ADMIN_PORTS_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Station Test Controls</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:24px 16px;font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#F5F0EB;color:#521B1B;min-height:100vh}"
    ".wrap{max-width:480px;margin:0 auto}"
    "h1{font-size:24px;margin:0 0 6px;letter-spacing:-0.01em}"
    ".sub{color:#6F1D1B;font-size:13px;margin:0 0 22px;opacity:0.75;line-height:1.45}"
    ".card{background:#fff;border-radius:18px;padding:18px 20px;margin-bottom:16px;"
    "box-shadow:0 6px 20px rgba(82,27,27,0.08);border:1px solid rgba(82,27,27,0.06)}"
    ".card h2{margin:0 0 10px;font-size:15px;font-weight:700;color:#521B1B;letter-spacing:0.01em;text-transform:uppercase}"
    ".row{display:flex;align-items:center;justify-content:space-between;padding:12px 0;"
    "border-bottom:1px solid rgba(82,27,27,0.06)}"
    ".row:last-child{border-bottom:0}"
    ".row label.name{font-size:15px;font-weight:500}"
    ".row .rhs{display:flex;align-items:center;gap:10px}"
    ".state{font-size:12px;color:#6F1D1B;opacity:0.6;min-width:62px;text-align:right}"
    ".state.on{color:#6F1D1B;opacity:1;font-weight:600}"
    /* toggle switch */
    ".switch{position:relative;display:inline-block;width:44px;height:24px;flex-shrink:0}"
    ".switch input{opacity:0;width:0;height:0}"
    ".slider{position:absolute;cursor:pointer;inset:0;background:#D4C4B0;border-radius:999px;"
    "transition:background 0.2s ease}"
    ".slider:before{position:absolute;content:'';height:18px;width:18px;left:3px;top:3px;"
    "background:#fff;border-radius:50%;transition:transform 0.2s ease;"
    "box-shadow:0 2px 4px rgba(0,0,0,0.15)}"
    ".switch input:checked + .slider{background:#6F1D1B}"
    ".switch input:checked + .slider:before{transform:translateX(20px)}"
    /* range slider */
    ".battery{display:flex;align-items:center;gap:14px;padding:8px 0}"
    ".battery input[type=range]{flex:1;-webkit-appearance:none;appearance:none;height:8px;"
    "border-radius:999px;background:linear-gradient(90deg,#6F1D1B 0%,#6F1D1B var(--val,60%),#D4C4B0 var(--val,60%),#D4C4B0 100%);outline:none}"
    ".battery input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;"
    "width:22px;height:22px;border-radius:50%;background:#6F1D1B;cursor:pointer;"
    "box-shadow:0 2px 6px rgba(0,0,0,0.2);border:3px solid #fff}"
    ".battery input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;"
    "background:#6F1D1B;cursor:pointer;border:3px solid #fff;box-shadow:0 2px 6px rgba(0,0,0,0.2)}"
    ".battery .val{font-size:18px;font-weight:700;min-width:52px;text-align:right;font-variant-numeric:tabular-nums}"
    /* toast */
    ".toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(20px);"
    "background:#262626;color:#fff;padding:10px 18px;border-radius:999px;font-size:13px;"
    "opacity:0;pointer-events:none;transition:opacity 0.22s ease,transform 0.22s ease;"
    "box-shadow:0 6px 20px rgba(0,0,0,0.25)}"
    ".toast.show{opacity:1;transform:translateX(-50%) translateY(0)}"
    ".toast.err{background:#6F1D1B}"
    "</style></head><body>"
    "<div class='wrap'>"
    "<h1>Station Test Controls</h1>"
    "<p class='sub'>Toggle a port to mark it in-use. Slide to simulate battery level. "
    "Each change is pushed to Supabase immediately.</p>"
    "<div class='card'>"
    "<h2>Ports</h2>"
    "<div class='row'><label class='name'>USB-A 1</label>"
    "<div class='rhs'><span class='state' data-for='usb_a_1'>Available</span>"
    "<label class='switch'><input type='checkbox' data-port='usb_a_1'><span class='slider'></span></label></div></div>"
    "<div class='row'><label class='name'>USB-A 2</label>"
    "<div class='rhs'><span class='state' data-for='usb_a_2'>Available</span>"
    "<label class='switch'><input type='checkbox' data-port='usb_a_2'><span class='slider'></span></label></div></div>"
    "<div class='row'><label class='name'>USB-C 1</label>"
    "<div class='rhs'><span class='state' data-for='usb_c_1'>Available</span>"
    "<label class='switch'><input type='checkbox' data-port='usb_c_1'><span class='slider'></span></label></div></div>"
    "<div class='row'><label class='name'>USB-C 2</label>"
    "<div class='rhs'><span class='state' data-for='usb_c_2'>Available</span>"
    "<label class='switch'><input type='checkbox' data-port='usb_c_2'><span class='slider'></span></label></div></div>"
    "<div class='row'><label class='name'>Outlet</label>"
    "<div class='rhs'><span class='state' data-for='outlet'>Available</span>"
    "<label class='switch'><input type='checkbox' data-port='outlet'><span class='slider'></span></label></div></div>"
    "</div>"
    "<div class='card'>"
    "<h2>Battery</h2>"
    "<div class='battery'>"
    "<input type='range' id='battery' min='0' max='100' value='60'>"
    "<span class='val' id='battery-val'>60%</span>"
    "</div>"
    "</div>"
    "</div>"
    "<div id='toast' class='toast'></div>"
    "<script>"
    "const toast=document.getElementById('toast');"
    "function notify(msg,err){toast.textContent=msg;toast.className='toast show'+(err?' err':'');"
    "clearTimeout(window.__t);window.__t=setTimeout(()=>{toast.className='toast';},1800)}"
    "async function post(path,body){try{const r=await fetch(path,{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
    "if(!r.ok){notify('Sync failed ('+r.status+')',true);return false}notify('Sent');return true}"
    "catch(e){notify('Network error',true);return false}}"
    "document.querySelectorAll('input[type=checkbox][data-port]').forEach(el=>{"
    "el.addEventListener('change',async()=>{const key=el.dataset.port;"
    "const status=el.checked?'in_use':'available';"
    "const label=document.querySelector('.state[data-for=\"'+key+'\"]');"
    "if(label){label.textContent=el.checked?'In use':'Available';label.classList.toggle('on',el.checked)}"
    "const ok=await post('/api/ports',{port_key:key,status:status});"
    "if(!ok){el.checked=!el.checked;if(label){label.textContent=el.checked?'In use':'Available';"
    "label.classList.toggle('on',el.checked)}}})});"
    "const bat=document.getElementById('battery'),batVal=document.getElementById('battery-val');"
    "function paint(){bat.style.setProperty('--val',bat.value+'%');batVal.textContent=bat.value+'%'}"
    "paint();bat.addEventListener('input',paint);"
    "bat.addEventListener('change',()=>post('/api/battery',{battery_percent:parseInt(bat.value,10)}));"
    "</script>"
    "</body></html>";

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */
static int read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= buf_len) {
        return -1;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return -1;
        }
        received += r;
    }
    buf[received] = '\0';
    return received;
}

/* Very small JSON-string-field extractor. Finds "key":"value" and copies
 * value into out. Does not handle escapes — fine for the short fixed
 * identifiers we send from our own HTML form. */
static bool json_str_field(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return false;
    }
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + n, ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    const char *end = strchr(p, '"');
    if (end == NULL) {
        return false;
    }
    size_t len = (size_t)(end - p);
    if (len >= out_len) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_int_field(const char *json, const char *key, int *out)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return false;
    }
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + n, ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *out = atoi(p);
    return true;
}

static bool is_known_port(const char *key)
{
    return strcmp(key, "usb_a_1") == 0
        || strcmp(key, "usb_a_2") == 0
        || strcmp(key, "usb_c_1") == 0
        || strcmp(key, "usb_c_2") == 0
        || strcmp(key, "outlet") == 0;
}

static bool is_known_status(const char *status)
{
    return strcmp(status, "available") == 0
        || strcmp(status, "in_use") == 0
        || strcmp(status, "offline") == 0
        || strcmp(status, "fault") == 0;
}

/* ------------------------------------------------------------------------
 * Handlers
 * ------------------------------------------------------------------------ */
static esp_err_t ports_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, ADMIN_PORTS_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_err(httpd_req_t *req, const char *code, const char *msg)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, code);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t api_ports_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) <= 0) {
        return send_json_err(req, "400 Bad Request", "body required");
    }

    char port_key[32] = {0};
    char status[24] = {0};
    if (!json_str_field(body, "port_key", port_key, sizeof(port_key))
        || !json_str_field(body, "status", status, sizeof(status))) {
        return send_json_err(req, "400 Bad Request", "need port_key and status");
    }
    if (!is_known_port(port_key)) {
        return send_json_err(req, "400 Bad Request", "unknown port_key");
    }
    if (!is_known_status(status)) {
        return send_json_err(req, "400 Bad Request", "unknown status");
    }

    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"station_id\":\"" STATION_ID "\",\"port_key\":\"%s\",\"status\":\"%s\"}",
             port_key, status);

    ESP_LOGI(TAG, "port update: %s -> %s", port_key, status);
    esp_err_t err = supabase_post_upsert(
        "/rest/v1/port_state?on_conflict=station_id,port_key",
        payload);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    ESP_LOGW(TAG, "supabase port upsert failed: %d", err);
    return send_json_err(req, "502 Bad Gateway", "supabase post failed");
}

static esp_err_t api_battery_handler(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) <= 0) {
        return send_json_err(req, "400 Bad Request", "body required");
    }

    int pct = -1;
    if (!json_int_field(body, "battery_percent", &pct)) {
        return send_json_err(req, "400 Bad Request", "need battery_percent");
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"station_id\":\"" STATION_ID "\",\"battery_percent\":%d}",
             pct);

    ESP_LOGI(TAG, "battery update: %d%%", pct);
    esp_err_t err = supabase_post_upsert(
        "/rest/v1/station_state?on_conflict=station_id",
        payload);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    ESP_LOGW(TAG, "supabase battery upsert failed: %d", err);
    return send_json_err(req, "502 Bad Gateway", "supabase post failed");
}

void admin_ports_register_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        ESP_LOGW(TAG, "admin_ports_register_handlers called with NULL server");
        return;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/ports",
        .method = HTTP_GET,
        .handler = ports_page_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t ports_api_uri = {
        .uri = "/api/ports",
        .method = HTTP_POST,
        .handler = api_ports_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t battery_api_uri = {
        .uri = "/api/battery",
        .method = HTTP_POST,
        .handler = api_battery_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &page_uri);
    httpd_register_uri_handler(server, &ports_api_uri);
    httpd_register_uri_handler(server, &battery_api_uri);

    ESP_LOGI(TAG, "admin /ports handlers registered");
}
