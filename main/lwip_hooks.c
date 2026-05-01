#include "lwip_hooks.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"

#include "client_acl.h"
#include "router_globals.h"

#define DHCP_OPTION_CAPTIVE_PORTAL 114
#define DHCP_STATE_OFFER 2
#define DHCP_STATE_ACK 5
#define DNS_PORT 53
#define HTTP_PORT 80

static const char *TAG = "SOLAR_LWIP_HOOK";

static void checksum_adjust(uint8_t *chksum, const uint8_t *old_data, int old_len, const uint8_t *new_data, int new_len)
{
    int32_t x;

    x = (chksum[0] * 256) + chksum[1];
    x = ~x & 0xFFFF;

    while (old_len > 0) {
        int32_t before = (old_data[0] * 256) + old_data[1];
        old_data += 2;
        x -= before & 0xFFFF;
        if (x <= 0) {
            x--;
            x &= 0xFFFF;
        }
        old_len -= 2;
    }

    while (new_len > 0) {
        int32_t after = (new_data[0] * 256) + new_data[1];
        new_data += 2;
        x += after & 0xFFFF;
        if (x & 0x10000) {
            x++;
            x &= 0xFFFF;
        }
        new_len -= 2;
    }

    x = ~x & 0xFFFF;
    chksum[0] = (uint8_t)(x / 256);
    chksum[1] = (uint8_t)(x & 0xFF);
}

static bool rewrite_unauthenticated_probe_to_portal(struct pbuf *p, struct ip_hdr *header, uint32_t old_dst)
{
    uint16_t ip_header_len;
    uint16_t dest_port;
    uint32_t new_dst = my_ap_ip;

    ip_header_len = IPH_HL_BYTES(header);
    if (ip_header_len < sizeof(struct ip_hdr) || p->len < ip_header_len) {
        return false;
    }

    if (IPH_OFFSET_BYTES(header) != 0) {
        return false;
    }

    if (IPH_PROTO(header) == IP_PROTO_TCP) {
        struct tcp_hdr *tcp_header;

        if (p->len < ip_header_len + sizeof(struct tcp_hdr)) {
            return false;
        }

        tcp_header = (struct tcp_hdr *)((uint8_t *)p->payload + ip_header_len);
        dest_port = lwip_ntohs(tcp_header->dest);
        if (dest_port != HTTP_PORT) {
            return false;
        }

        checksum_adjust((uint8_t *)&tcp_header->chksum,
                        (const uint8_t *)&old_dst,
                        sizeof(old_dst),
                        (const uint8_t *)&new_dst,
                        sizeof(new_dst));
    } else if (IPH_PROTO(header) == IP_PROTO_UDP) {
        struct udp_hdr *udp_header;

        if (p->len < ip_header_len + sizeof(struct udp_hdr)) {
            return false;
        }

        udp_header = (struct udp_hdr *)((uint8_t *)p->payload + ip_header_len);
        dest_port = lwip_ntohs(udp_header->dest);
        if (dest_port != DNS_PORT) {
            return false;
        }

        if (udp_header->chksum != 0) {
            checksum_adjust((uint8_t *)&udp_header->chksum,
                            (const uint8_t *)&old_dst,
                            sizeof(old_dst),
                            (const uint8_t *)&new_dst,
                            sizeof(new_dst));
        }
    } else {
        return false;
    }

    checksum_adjust((uint8_t *)&IPH_CHKSUM(header),
                    (const uint8_t *)&old_dst,
                    sizeof(old_dst),
                    (const uint8_t *)&new_dst,
                    sizeof(new_dst));
    header->dest.addr = new_dst;

    ESP_LOGI(TAG, "Intercepted unauthenticated %s/%u probe to captive portal",
             IPH_PROTO(header) == IP_PROTO_TCP ? "tcp" : "udp",
             dest_port);

    return true;
}

int solar_hook_ip4_input(struct pbuf *p, struct netif *inp)
{
    struct ip_hdr *header;
    uint32_t src;
    uint32_t dst;
    ip4_addr_t dst_addr;

    if (p == NULL || inp == NULL || p->len < sizeof(struct ip_hdr)) {
        return 0;
    }

    if (netif_ip4_addr(inp)->addr != my_ap_ip) {
        return 0;
    }

    header = (struct ip_hdr *)p->payload;
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

    if (rewrite_unauthenticated_probe_to_portal(p, header, dst)) {
        return 0;
    }

    pbuf_free(p);
    return 1;
}

void solar_hook_dhcps_post_append_opts(struct netif *netif, void *dhcps, int state, unsigned char **pp_opts)
{
    ip4_addr_t ap_addr;
    char ap_ip[16];
    char portal_url[32];
    size_t portal_url_len;
    unsigned char *opt;

    (void)dhcps;

    if (netif == NULL || pp_opts == NULL || *pp_opts == NULL) {
        return;
    }

    if (state != DHCP_STATE_OFFER && state != DHCP_STATE_ACK) {
        return;
    }

    if (my_ap_ip == 0 || netif_ip4_addr(netif)->addr != my_ap_ip) {
        return;
    }

    ap_addr.addr = my_ap_ip;
    if (ip4addr_ntoa_r(&ap_addr, ap_ip, sizeof(ap_ip)) == NULL) {
        return;
    }

    snprintf(portal_url, sizeof(portal_url), "http://%s/", ap_ip);
    portal_url_len = strlen(portal_url);
    if (portal_url_len == 0 || portal_url_len > 255) {
        return;
    }

    opt = *pp_opts;
    *opt++ = DHCP_OPTION_CAPTIVE_PORTAL;
    *opt++ = (unsigned char)portal_url_len;
    memcpy(opt, portal_url, portal_url_len);
    opt += portal_url_len;
    *pp_opts = opt;

    ESP_LOGI(TAG, "Advertising DHCP captive portal option 114: %s", portal_url);
}
