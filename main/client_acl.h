#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void client_acl_init(void);
void client_acl_clear(void);
void client_acl_admit(uint32_t ip_net, const uint8_t *mac);
void client_acl_revoke_by_ip(uint32_t ip_net);
void client_acl_revoke_by_mac(const uint8_t *mac);
bool client_acl_is_admitted(uint32_t ip_net);

#ifdef __cplusplus
}
#endif
