#include "admin_ports.h"
#include "port_sensors.h"
#include "supabase_client.h"

#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "admin_ports";

#define STATION_ID "solar-hub-01"

static void register_admin_uri_checked(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err;

    if (server == NULL || uri == NULL) {
        return;
    }

    err = httpd_register_uri_handler(server, uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered handler %s", uri->uri);
    } else {
        ESP_LOGE(TAG, "failed to register handler %s: %s", uri->uri, esp_err_to_name(err));
    }
}

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
 * /port-occupied — live USB-A port occupancy view
 *
 * Polls /api/ports/sensors and renders only usb_a_1 (INA219 0x44) and
 * usb_a_2 (INA219 0x45). The USB-C INA219s (0x40, 0x41) are intentionally
 * skipped because those chips are faulty on the current PCB rev (their
 * SCL pins are internally shorted to GND).
 * ------------------------------------------------------------------------ */
static const char *PORT_OCCUPIED_HTML =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Port Occupancy</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:24px 16px;font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#F5F0EB;color:#521B1B;min-height:100vh}"
    ".wrap{max-width:480px;margin:0 auto}"
    "h1{font-size:24px;margin:0 0 6px;letter-spacing:-0.01em}"
    ".sub{color:#6F1D1B;font-size:13px;margin:0 0 22px;opacity:0.75;line-height:1.45}"
    ".card{background:#fff;border-radius:18px;padding:18px 20px;margin-bottom:14px;"
    "box-shadow:0 6px 20px rgba(82,27,27,0.08);border:2px solid rgba(82,27,27,0.06);"
    "transition:border-color 0.2s ease}"
    ".card.available{border-color:#86efac}"
    ".card.in-use{border-color:#fca5a5}"
    ".card.fault,.card.not-ready{border-color:#fbbf24}"
    ".card h2{margin:0 0 10px;font-size:14px;font-weight:700;color:#521B1B;letter-spacing:0.04em;text-transform:uppercase}"
    ".badge{display:inline-block;padding:6px 14px;border-radius:999px;font-size:13px;font-weight:700;letter-spacing:0.04em}"
    ".badge.available{background:#dcfce7;color:#166534}"
    ".badge.in-use{background:#fee2e2;color:#991b1b}"
    ".badge.fault,.badge.not-ready{background:#fef3c7;color:#92400e}"
    ".reading{margin-top:10px;font-size:13px;color:#6F1D1B;opacity:0.85;font-variant-numeric:tabular-nums}"
    ".reading b{color:#521B1B;opacity:1;font-weight:700}"
    ".footer{color:#6F1D1B;font-size:12px;opacity:0.6;margin-top:16px;text-align:center}"
    ".error{color:#991b1b;background:#fee2e2;padding:10px 14px;border-radius:10px;font-size:13px;margin-bottom:14px;display:none}"
    "</style></head><body>"
    "<div class='wrap'>"
    "<h1>USB-A Port Occupancy</h1>"
    "<p class='sub'>Live readings from the INA219 current sensors. "
    "Above 50 mA is treated as in use.</p>"
    "<div id='err' class='error'></div>"
    "<div class='card not-ready' data-key='usb_a_1'>"
    "<h2>USB-A 1</h2>"
    "<span class='badge not-ready'>connecting…</span>"
    "<div class='reading'>—</div>"
    "</div>"
    "<div class='card not-ready' data-key='usb_a_2'>"
    "<h2>USB-A 2</h2>"
    "<span class='badge not-ready'>connecting…</span>"
    "<div class='reading'>—</div>"
    "</div>"
    "<div class='footer' id='ftr'>refreshing every 1.5 s</div>"
    "</div>"
    "<script>"
    "var TARGETS=['usb_a_1','usb_a_2'];"
    "var LABELS={available:'AVAILABLE',in_use:'IN USE',fault:'FAULT',not_ready:'NOT READY'};"
    "function fmt(n,d){return (typeof n==='number'?n:0).toFixed(d);}"
    "async function tick(){"
    "try{"
    "var r=await fetch('/api/ports/sensors',{cache:'no-store'});"
    "var j=await r.json();"
    "if(!j||!j.ports)throw new Error('bad response');"
    "document.getElementById('err').style.display='none';"
    "for(var i=0;i<j.ports.length;i++){"
    "var p=j.ports[i];"
    "if(TARGETS.indexOf(p.port_key)<0)continue;"
    "var card=document.querySelector(\".card[data-key='\"+p.port_key+\"']\");"
    "if(!card)continue;"
    "var s=(p.status||'not_ready').toLowerCase();"
    "var cls=(s==='in_use')?'in-use':(s==='available')?'available':(s==='fault')?'fault':'not-ready';"
    "card.className='card '+cls;"
    "var b=card.querySelector('.badge');"
    "b.className='badge '+cls;"
    "b.textContent=LABELS[s]||s.toUpperCase();"
    "var rd=card.querySelector('.reading');"
    "rd.innerHTML='<b>'+fmt(p.current_ma,1)+' mA</b> at '+fmt(p.bus_voltage_v,2)+' V';"
    "}"
    "document.getElementById('ftr').textContent='updated '+new Date().toLocaleTimeString();"
    "}catch(e){"
    "var ee=document.getElementById('err');"
    "ee.textContent='Lost connection: '+e.message;"
    "ee.style.display='block';"
    "}"
    "}"
    "tick();setInterval(tick,1500);"
    "</script></body></html>";

/* ------------------------------------------------------------------------
 * Handlers
 * ------------------------------------------------------------------------ */
static esp_err_t ports_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, ADMIN_PORTS_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t port_occupied_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORT_OCCUPIED_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_err(httpd_req_t *req, const char *code, const char *msg)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, code);
    return httpd_resp_sendstr(req, body);
}

static void json_append(char *buf, size_t buf_len, size_t *offset, const char *fmt, ...)
{
    va_list args;
    int written;

    if (buf == NULL || offset == NULL || *offset >= buf_len) {
        return;
    }

    va_start(args, fmt);
    written = vsnprintf(buf + *offset, buf_len - *offset, fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    if ((size_t)written >= buf_len - *offset) {
        *offset = buf_len - 1;
        buf[*offset] = '\0';
        return;
    }

    *offset += (size_t)written;
}

static esp_err_t api_ports_i2c_scan_handler(httpd_req_t *req)
{
    uint8_t detected[32];
    size_t count = 0;
    esp_err_t err = port_sensors_scan_i2c(detected, sizeof(detected), &count);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_err(req,
                             "409 Conflict",
                             "port sensors disabled; build with PORT_SENSORS_ENABLED=1");
    }
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "i2c scan failed: %s", esp_err_to_name(err));
        return send_json_err(req, "500 Internal Server Error", msg);
    }

    char body[768];
    size_t off = 0;
    json_append(body, sizeof(body), &off,
                "{\"ok\":true,\"enabled\":true,"
                "\"sda_gpio\":%d,\"scl_gpio\":%d,"
                "\"expected\":[\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\"],"
                "\"detected\":[",
                PORT_SENSORS_I2C_PIN_SDA,
                PORT_SENSORS_I2C_PIN_SCL,
                PORT_SENSORS_INA219_ADDR_USB_C_1,
                PORT_SENSORS_INA219_ADDR_USB_C_2,
                PORT_SENSORS_INA219_ADDR_USB_A_1,
                PORT_SENSORS_INA219_ADDR_USB_A_2);

    for (size_t i = 0; i < count && i < sizeof(detected); i++) {
        json_append(body, sizeof(body), &off,
                    "%s\"0x%02X\"",
                    i == 0 ? "" : ",",
                    detected[i]);
    }

    json_append(body, sizeof(body), &off, "],\"count\":%u}", (unsigned)count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t api_ports_sensor_readings_handler(httpd_req_t *req)
{
    port_sensor_reading_t readings[PORT_SENSOR_COUNT];
    esp_err_t err = port_sensors_read_all(readings);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_err(req,
                             "409 Conflict",
                             "port sensors disabled; build with PORT_SENSORS_ENABLED=1");
    }

    char body[1280];
    size_t off = 0;
    json_append(body, sizeof(body), &off,
                "{\"ok\":%s,\"enabled\":%s,\"threshold_ma\":%.1f",
                err == ESP_OK ? "true" : "false",
                port_sensors_is_enabled() ? "true" : "false",
                (double)PORT_IN_USE_THRESHOLD_MA);
    if (err != ESP_OK) {
        json_append(body, sizeof(body), &off,
                    ",\"read_error\":\"%s\"",
                    esp_err_to_name(err));
    }
    json_append(body, sizeof(body), &off, ",\"ports\":[");

    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        const port_sensor_reading_t *r = &readings[i];
        json_append(body, sizeof(body), &off,
                    "%s{\"port_key\":\"%s\",\"address\":\"0x%02X\","
                    "\"mosfet_gpio\":%d,\"current_ma\":%.1f,"
                    "\"bus_voltage_v\":%.3f,\"status\":\"%s\"}",
                    i == 0 ? "" : ",",
                    r->port_key != NULL ? r->port_key : "",
                    r->i2c_address,
                    r->mosfet_gpio,
                    (double)r->current_ma,
                    (double)r->bus_voltage_v,
                    port_sensors_status_string(r->status));
    }

    json_append(body, sizeof(body), &off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t api_ports_sensor_sync_handler(httpd_req_t *req)
{
    esp_err_t err = port_sensors_sync_once();

    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_err(req,
                             "409 Conflict",
                             "port sensors disabled; build with PORT_SENSORS_ENABLED=1");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    char body[96];
    snprintf(body,
             sizeof(body),
             "{\"ok\":false,\"error\":\"sensor sync failed: %s\"}",
             esp_err_to_name(err));
    httpd_resp_set_status(req, "502 Bad Gateway");
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
    static const httpd_uri_t port_occupied_uri = {
        .uri = "/port-occupied",
        .method = HTTP_GET,
        .handler = port_occupied_page_handler,
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
    static const httpd_uri_t i2c_scan_api_uri = {
        .uri = "/api/ports/i2c-scan",
        .method = HTTP_GET,
        .handler = api_ports_i2c_scan_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t sensor_readings_api_uri = {
        .uri = "/api/ports/sensors",
        .method = HTTP_GET,
        .handler = api_ports_sensor_readings_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t sensor_sync_api_uri = {
        .uri = "/api/ports/sensors/sync",
        .method = HTTP_POST,
        .handler = api_ports_sensor_sync_handler,
        .user_ctx = NULL,
    };

    register_admin_uri_checked(server, &page_uri);
    register_admin_uri_checked(server, &port_occupied_uri);
    register_admin_uri_checked(server, &ports_api_uri);
    register_admin_uri_checked(server, &battery_api_uri);
    register_admin_uri_checked(server, &i2c_scan_api_uri);
    register_admin_uri_checked(server, &sensor_readings_api_uri);
    register_admin_uri_checked(server, &sensor_sync_api_uri);

    ESP_LOGI(TAG, "admin /ports handlers registered");
}
