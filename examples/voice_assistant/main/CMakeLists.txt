idf_component_register(SRCS "main.c" "assistant_protocol.c" "assistant_network.c" "assistant_wifi.c"
    "assistant_agent.c" "assistant_http_tools.c" "assistant_rgb.c" "assistant_rgb_pattern.c" "asr_display.c" "asr_screen.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lingasr lingtts esp_driver_i2s esp_driver_spi esp_driver_uart esp_driver_gpio esp_driver_rmt
        esp_lcd esp_timer heap esp_partition mbedtls esp_wifi esp_netif esp_event esp_http_server esp_http_client nvs_flash json lwip)
idf_build_get_property(portal_python PYTHON)
set(portal_gzip "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${portal_gzip}"
    COMMAND "${portal_python}" "${CMAKE_CURRENT_LIST_DIR}/compress_portal.py" "${CMAKE_CURRENT_LIST_DIR}/index.html" "${portal_gzip}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/index.html" "${CMAKE_CURRENT_LIST_DIR}/compress_portal.py")
add_custom_target(assistant_portal DEPENDS "${portal_gzip}")
add_dependencies(${COMPONENT_LIB} assistant_portal)
target_add_binary_data(${COMPONENT_LIB} "${portal_gzip}" BINARY)
