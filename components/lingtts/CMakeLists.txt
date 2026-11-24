# LingTTS 预编译组件（ESP32-S3）。把整个 components/lingtts 目录放进工程的 components/，
# 或者在工程 CMakeLists.txt 里 set(EXTRA_COMPONENT_DIRS ".../components/lingtts")。
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "LingTTS 预编译库只支持 esp32s3（当前 IDF_TARGET = ${IDF_TARGET}）")
endif()
idf_component_register(INCLUDE_DIRS "include")
add_prebuilt_library(lingtts_prebuilt "${CMAKE_CURRENT_LIST_DIR}/lib/esp32s3/liblingtts.a"
    PRIV_REQUIRES esp_timer heap esp_partition spi_flash efuse esp_hw_support)
target_link_libraries(${COMPONENT_LIB} INTERFACE lingtts_prebuilt)
