if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "LingASR Preview requires ESP32-S3")
endif()
idf_component_register(INCLUDE_DIRS "include")
add_prebuilt_library(lingasr_prebuilt "${CMAKE_CURRENT_LIST_DIR}/lib/esp32s3/liblingasr.a"
    PRIV_REQUIRES esp_timer heap esp_partition spi_flash mbedtls freertos)
target_link_libraries(${COMPONENT_LIB} INTERFACE lingasr_prebuilt)
