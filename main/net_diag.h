#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void net_diag_start_task(void);
void net_diag_log_snapshot(const char *reason);
void net_diag_schedule_probe(const char *reason);
void net_diag_set_napt_state(bool enabled, esp_err_t last_err);
void net_diag_set_portal_state(bool authenticated, const char *reason);

#ifdef __cplusplus
}
#endif
