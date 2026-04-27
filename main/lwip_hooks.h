#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct pbuf;
struct netif;

int solar_hook_ip4_input(struct pbuf *p, struct netif *inp);

#define LWIP_HOOK_IP4_INPUT(p, inp) solar_hook_ip4_input((p), (inp))

#ifdef __cplusplus
}
#endif
