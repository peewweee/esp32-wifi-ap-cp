/* Various global declarations for the esp32_nat_router

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h" // For esp_netif_t

#define PARAM_NAMESPACE "esp32_nat"

#define PROTO_TCP 6
#define PROTO_UDP 17

// --- Variables for Admin Config Page ---
extern char* ssid;
extern char* ent_username;
extern char* ent_identity;
extern char* passwd;
extern char* static_ip;
extern char* subnet_mask;
extern char* gateway_addr;
extern char* ap_ssid;
extern char* ap_passwd;

extern uint16_t connect_count;
extern bool ap_connect;
extern uint8_t last_ap_client_mac[6];
extern bool last_ap_client_mac_valid;

extern uint32_t my_ip;
extern uint32_t my_ap_ip;

// --- NEW Variables for Portal ---
extern bool portal_authenticated;
extern esp_netif_t* wifiAP;
extern esp_netif_t* wifiSTA;


// --- Function Declarations ---
bool is_client_session_active(uint32_t ip_addr);
void preprocess_string(char* str);
int set_sta(int argc, char **argv);
int set_sta_static(int argc, char **argv);
int set_ap(int argc, char **argv);

esp_err_t get_config_param_blob(char* name, uint8_t** blob, size_t blob_len);
esp_err_t get_config_param_int(char* name, int* param);
esp_err_t get_config_param_str(char* name, char** param);

void print_portmap_tab();
esp_err_t add_portmap(uint8_t proto, uint16_t mport, uint32_t daddr, uint16_t dport);
esp_err_t del_portmap(uint8_t proto, uint16_t mport);
void handle_client_connect(const uint8_t mac[6]);
void handle_client_disconnect(const uint8_t mac[6]);
void handle_client_ip_assigned(const uint8_t mac[6], uint32_t ip_net);

/* Battery state machine action hooks. */
void set_user_ap_enabled(bool enabled);
void rfid_reader_set_ports_allowed(bool allowed);

#ifdef __cplusplus
}
#endif
