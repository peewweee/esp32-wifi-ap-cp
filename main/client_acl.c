#include "client_acl.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#define ACL_MAX_CLIENTS 20

typedef struct {
    bool used;
    uint32_t ip;
    uint8_t mac[6];
    bool has_mac;
} acl_entry_t;

static acl_entry_t s_acl[ACL_MAX_CLIENTS];
static portMUX_TYPE s_acl_lock = portMUX_INITIALIZER_UNLOCKED;

static bool mac_matches(const acl_entry_t *entry, const uint8_t *mac)
{
    return entry != NULL &&
           entry->has_mac &&
           mac != NULL &&
           memcmp(entry->mac, mac, 6) == 0;
}

void client_acl_init(void)
{
    portENTER_CRITICAL(&s_acl_lock);
    memset(s_acl, 0, sizeof(s_acl));
    portEXIT_CRITICAL(&s_acl_lock);
}

void client_acl_clear(void)
{
    client_acl_init();
}

void client_acl_admit(uint32_t ip_net, const uint8_t *mac)
{
    if (ip_net == 0) {
        return;
    }

    portENTER_CRITICAL(&s_acl_lock);

    if (mac != NULL) {
        for (int i = 0; i < ACL_MAX_CLIENTS; i++) {
            if (s_acl[i].used && mac_matches(&s_acl[i], mac)) {
                s_acl[i].ip = ip_net;
                portEXIT_CRITICAL(&s_acl_lock);
                return;
            }
        }
    }

    for (int i = 0; i < ACL_MAX_CLIENTS; i++) {
        if (s_acl[i].used && s_acl[i].ip == ip_net) {
            if (mac != NULL) {
                memcpy(s_acl[i].mac, mac, sizeof(s_acl[i].mac));
                s_acl[i].has_mac = true;
            }
            portEXIT_CRITICAL(&s_acl_lock);
            return;
        }
    }

    for (int i = 0; i < ACL_MAX_CLIENTS; i++) {
        if (!s_acl[i].used) {
            s_acl[i].used = true;
            s_acl[i].ip = ip_net;
            if (mac != NULL) {
                memcpy(s_acl[i].mac, mac, sizeof(s_acl[i].mac));
                s_acl[i].has_mac = true;
            }
            break;
        }
    }

    portEXIT_CRITICAL(&s_acl_lock);
}

void client_acl_revoke_by_ip(uint32_t ip_net)
{
    if (ip_net == 0) {
        return;
    }

    portENTER_CRITICAL(&s_acl_lock);
    for (int i = 0; i < ACL_MAX_CLIENTS; i++) {
        if (s_acl[i].used && s_acl[i].ip == ip_net) {
            memset(&s_acl[i], 0, sizeof(s_acl[i]));
        }
    }
    portEXIT_CRITICAL(&s_acl_lock);
}

void client_acl_revoke_by_mac(const uint8_t *mac)
{
    if (mac == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_acl_lock);
    for (int i = 0; i < ACL_MAX_CLIENTS; i++) {
        if (s_acl[i].used && mac_matches(&s_acl[i], mac)) {
            memset(&s_acl[i], 0, sizeof(s_acl[i]));
        }
    }
    portEXIT_CRITICAL(&s_acl_lock);
}

bool client_acl_is_admitted(uint32_t ip_net)
{
    bool admitted = false;

    if (ip_net == 0) {
        return false;
    }

    portENTER_CRITICAL(&s_acl_lock);
    for (int i = 0; i < ACL_MAX_CLIENTS; i++) {
        if (s_acl[i].used && s_acl[i].ip == ip_net) {
            admitted = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_acl_lock);

    return admitted;
}
