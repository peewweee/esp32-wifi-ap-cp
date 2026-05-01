#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct pbuf;
struct netif;

int solar_hook_ip4_input(struct pbuf *p, struct netif *inp);
void solar_hook_dhcps_post_append_opts(struct netif *netif, void *dhcps, int state, unsigned char **pp_opts);

#define LWIP_HOOK_IP4_INPUT(p, inp) solar_hook_ip4_input((p), (inp))
#define LWIP_HOOK_DHCPS_POST_APPEND_OPTS(netif, dhcps, state, pp_opts) \
    solar_hook_dhcps_post_append_opts((netif), (dhcps), (state), (unsigned char **)(pp_opts));

#ifdef __cplusplus
}
#endif
