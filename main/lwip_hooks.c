#include "lwip_hooks.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"

#include "client_acl.h"
#include "router_globals.h"

int solar_hook_ip4_input(struct pbuf *p, struct netif *inp)
{
    const struct ip_hdr *header;
    uint32_t src;
    uint32_t dst;
    ip4_addr_t dst_addr;

    if (p == NULL || inp == NULL || p->len < sizeof(struct ip_hdr)) {
        return 0;
    }

    if (netif_ip4_addr(inp)->addr != my_ap_ip) {
        return 0;
    }

    header = (const struct ip_hdr *)p->payload;
    src = header->src.addr;
    dst = header->dest.addr;
    dst_addr.addr = dst;

    if (dst == my_ap_ip) {
        return 0;
    }

    if (ip4_addr_isbroadcast(&dst_addr, inp) || ip4_addr_ismulticast(&dst_addr)) {
        return 0;
    }

    if (src == 0 || client_acl_is_admitted(src)) {
        return 0;
    }

    pbuf_free(p);
    return 1;
}
