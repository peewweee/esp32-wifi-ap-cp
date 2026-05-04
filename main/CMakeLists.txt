idf_component_register(SRCS "pzem_reader.c" "lwip_hooks.c"
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
    ADMIN_DEFAULT_PASSWORD="${PROJECT_ADMIN_PASSWORD}"
    PORT_SENSORS_ENABLED=${PROJECT_PORT_SENSORS_ENABLED}
    PORT_SENSORS_SUPABASE_SYNC_ENABLED=${PROJECT_PORT_SENSORS_SUPABASE_SYNC_ENABLED})

if(NOT PROJECT_PORT_IN_USE_THRESHOLD_MA STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        PORT_IN_USE_THRESHOLD_MA=${PROJECT_PORT_IN_USE_THRESHOLD_MA})
endif()

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
    # Force the linker to pull hook implementations from libmain.a even though
    # liblwip.a is scanned first. Without these -u flags the references inside
    # ip4_input/dhcpserver stay unresolved at link time.
    target_link_libraries(${LWIP_LIB} INTERFACE
        "-u solar_hook_ip4_input"
        "-u solar_hook_dhcps_post_append_opts"
    )
endif()
