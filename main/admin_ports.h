#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers:
 *   GET  /ports         -> test admin page (HTML)
 *   POST /api/ports     -> {"port_key":"usb_a_1","status":"in_use"|"available"}
 *   POST /api/battery   -> {"battery_percent":75}
 *
 * Call this from start_webserver() BEFORE the catch-all handler is
 * registered so the specific URIs match first.
 */
void admin_ports_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
