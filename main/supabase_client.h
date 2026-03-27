#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void supabase_init(void);
esp_err_t supabase_create_session(const char *token, const char *mac_hash, int duration_sec);
esp_err_t supabase_update_heartbeat(const char *token, int remaining_sec);
esp_err_t supabase_mark_disconnected(const char *token);

#ifdef __cplusplus
}
#endif
