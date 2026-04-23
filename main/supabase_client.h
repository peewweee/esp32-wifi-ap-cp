#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void supabase_init(void);
esp_err_t supabase_create_session(const char *session_token, const char *device_hash, int duration_sec);
esp_err_t supabase_update_heartbeat(const char *session_token, const char *device_hash, int remaining_sec);
esp_err_t supabase_mark_disconnected(const char *session_token, const char *device_hash, int remaining_sec);

/* Generic upsert helper: POSTs json_body to a Supabase REST path with
 * Prefer: resolution=merge-duplicates. Path should include leading slash
 * and any ?on_conflict=... query, e.g.
 *   "/rest/v1/port_state?on_conflict=station_id,port_key"
 */
esp_err_t supabase_post_upsert(const char *path, const char *json_body);

#ifdef __cplusplus
}
#endif
