# ESP-IDF 在 project() 时自动包含各组件的 project_include.cmake。
# 工程的 CMakeLists.txt 在 project() 之后调用：
#   lingtts_flash_assets(<资源目录> [<license.bin>])
# idf.py flash 时把 <资源目录>/lingtts.bin、lingtts_fe.bin 写进 lingtts_model、lingtts_fe 分区，
# 给了 license.bin 就写进 lingtts_lic 分区（社区版无需此文件，默认不自动加载授权分区）。
function(lingtts_flash_assets asset_dir)
    foreach(pair "lingtts_model:lingtts.bin" "lingtts_fe:lingtts_fe.bin")
        string(REPLACE ":" ";" pair "${pair}")
        list(GET pair 0 part)
        list(GET pair 1 file)
        if(NOT EXISTS "${asset_dir}/${file}")
            message(FATAL_ERROR "缺少 ${asset_dir}/${file}：用 -D LINGTTS_ASSET_DIR=/绝对路径 指定资源目录")
        endif()
        esptool_py_flash_to_partition(flash "${part}" "${asset_dir}/${file}")
    endforeach()
    if(ARGC GREATER 1 AND ARGV1)
        if(NOT EXISTS "${ARGV1}")
            message(FATAL_ERROR "授权文件 ${ARGV1} 不存在")
        endif()
        esptool_py_flash_to_partition(flash "lingtts_lic" "${ARGV1}")
    endif()
endfunction()
