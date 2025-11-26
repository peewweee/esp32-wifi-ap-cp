#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h" // Required for gethostbyname
#include "esp_log.h"
#include <string.h>
#include "esp_netif.h"
#include <errno.h>

static const char *TAG_DNS = "DNS_SERVER";
#define DNS_PORT 53

// Global pointer to the AP interface (defined in esp32_nat_router.c)
extern esp_netif_t *wifiAP; 

// --- CRITICAL IMPORT: Check if user is allowed internet ---
extern int get_remaining_seconds(uint32_t ip);

// Helper: Convert DNS "3www6google3com0" format to "www.google.com"
// We need this so we can look up the REAL IP for authenticated users.
void parse_dns_name(char *buffer, char *out_name) {
    int i = 0;
    int j = 0;
    while (buffer[i] != 0) {
        int len = buffer[i];
        i++;
        for (int k = 0; k < len; k++) {
            out_name[j++] = buffer[i++];
        }
        out_name[j++] = '.';
    }
    if (j > 0) out_name[j-1] = '\0';
    else out_name[0] = '\0';
}

static void dns_server_task(void *pvParameters) {
    int sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char rx_buffer[512]; 

    // 1. Get the SoftAP IP Address (The Trap IP)
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(wifiAP, &ip_info);
    uint32_t captive_ip = ip_info.ip.addr;
    ESP_LOGI(TAG_DNS, "Captive IP Address: %" PRIu32, captive_ip);

    // 2. Create Socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG_DNS, "Failed to create socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY; 

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG_DNS, "Failed to bind socket: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG_DNS, "Smart DNS Server listening...");

    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &client_len);

        if (len >= 12) {
            uint32_t client_ip = client_addr.sin_addr.s_addr;
            
            // --- LOGIC: IS THIS SPECIFIC DEVICE ALLOWED INTERNET? ---
            int remaining = get_remaining_seconds(client_ip);
            
            uint32_t response_ip = captive_ip; // Default: TRAP THEM

            if (remaining > 0) {
                // ALLOWED! Try to find the REAL IP for them.
                char domain_name[256];
                // DNS header is 12 bytes, so name starts at index 12
                parse_dns_name(&rx_buffer[12], domain_name);
                
                ESP_LOGI(TAG_DNS, "Auth User (IP ends in .%d) asking for: %s", (int)((client_ip >> 24) & 0xFF), domain_name);
                
                struct hostent *he = gethostbyname(domain_name);
                if (he != NULL && he->h_addr_list != NULL && he->h_addr_list[0] != NULL) {
                    response_ip = ((struct in_addr *)(he->h_addr_list[0]))->s_addr;
                } else {
                    // If we can't find the real IP (internet down?), keep them trapped so they see a page.
                    ESP_LOGW(TAG_DNS, "Failed to resolve %s", domain_name);
                }
            } 
            // --------------------------------------------------------

            // Construct DNS Response (Standard A Record)
            // Modify flags for response: QR=1, AA=1, RA=1 -> 0x8180
            uint16_t flags = htons(0x8180); 
            memcpy(&rx_buffer[2], &flags, sizeof(flags));

            uint16_t ancount = htons(1);
            memcpy(&rx_buffer[6], &ancount, sizeof(ancount));

            uint16_t zero = htons(0);
            memcpy(&rx_buffer[8], &zero, sizeof(zero)); 
            memcpy(&rx_buffer[10], &zero, sizeof(zero)); 

            // Skip over the Question Section to find where to write the Answer
            int question_end = 12; 
            while (question_end < len && rx_buffer[question_end] != 0) {
                question_end += (rx_buffer[question_end] + 1);
            }
            question_end += 5; // Skip Null byte + QTYPE + QCLASS

            char *answer_ptr = rx_buffer + question_end;

            // 1. Name Ptr (0xC00C points to start of packet name)
            uint16_t name_ptr = htons(0xC00C);
            memcpy(answer_ptr, &name_ptr, 2); answer_ptr += 2;
            
            // 2. Type (A Record = 1)
            uint16_t type = htons(1);
            memcpy(answer_ptr, &type, 2); answer_ptr += 2;
            
            // 3. Class (IN = 1)
            uint16_t class = htons(1);
            memcpy(answer_ptr, &class, 2); answer_ptr += 2;
            
            // 4. TTL (60 seconds)
            uint32_t ttl = htonl(60); 
            memcpy(answer_ptr, &ttl, 4); answer_ptr += 4;
            
            // 5. Data Length (4 bytes for IPv4)
            uint16_t rdlen = htons(4);
            memcpy(answer_ptr, &rdlen, 2); answer_ptr += 2;
            
            // 6. The IP Address (Either Trap IP or Real IP)
            memcpy(answer_ptr, &response_ip, 4); answer_ptr += 4;

            int resp_len = answer_ptr - rx_buffer;
            sendto(sock, rx_buffer, resp_len, 0, (struct sockaddr *)&client_addr, client_len);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

void start_dns_server(void) {
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}