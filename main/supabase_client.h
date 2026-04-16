#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void supabase_init(void);
esp_err_t supabase_create_session(const char *session_token, const char *device_hash, int duration_sec);
esp_err_t supabase_update_heartbeat(const char *session_token, int remaining_sec);
esp_err_t supabase_mark_disconnected(const char *session_token);

#ifdef __cplusplus
}
#endif
