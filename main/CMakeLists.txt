idf_component_register(SRCS "lwip_hooks.c"
                            "client_acl.c"
                            "eco_metrics.c"
                            "port_sensors.c"
                            "admin_ports.c"
                            "rfid_reader.c"
                            "esp32_nat_router.c"
                            "http_server.c"
                            "dns_server.c"
                            "net_diag.c"
                            "supabase_client.c"
                    INCLUDE_DIRS "."
                    REQUIRES mbedtls
                    PRIV_REQUIRES esp_http_client
                                  esp_http_server
                                  esp_wifi
                                  esp_event
                                  esp_netif
                                  esp_timer
                                  nvs_flash
                                  console
                                  driver
                                  fatfs
                                  lwip
                                  spiffs
                                  vfs
                                  wpa_supplicant
                                  cmd_router
                                  cmd_nvs
                                  cmd_system)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SUPABASE_BASE_URL="${PROJECT_SUPABASE_BASE_URL}"
    SUPABASE_API_KEY="${PROJECT_SUPABASE_API_KEY}"
    ADMIN_DEFAULT_USERNAME="${PROJECT_ADMIN_USERNAME}"
    ADMIN_DEFAULT_PASSWORD="${PROJECT_ADMIN_PASSWORD}")

set_source_files_properties(http_server.c
    PROPERTIES COMPILE_FLAGS
    -Wno-unused-function
)

idf_component_get_property(LWIP_LIB lwip COMPONENT_LIB)
if(LWIP_LIB)
    target_compile_definitions(${LWIP_LIB} PUBLIC
        ESP_IDF_LWIP_HOOK_FILENAME="lwip_hooks.h"
    )
    target_include_directories(${LWIP_LIB} PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}"
    )
endif()
