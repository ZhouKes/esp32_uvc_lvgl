# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DEFINED LVGL_UVC_WIDTH)
    idf_build_set_property(COMPILE_DEFINITIONS "LVGL_UVC_WIDTH=${LVGL_UVC_WIDTH}" APPEND)
endif()
if(DEFINED LVGL_UVC_HEIGHT)
    idf_build_set_property(COMPILE_DEFINITIONS "LVGL_UVC_HEIGHT=${LVGL_UVC_HEIGHT}" APPEND)
endif()
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_USB_DWC2_CUSTOM_FIFO" APPEND)
idf_build_set_property(MINIMAL_BUILD ON)
project(cherryusb_serial_device)
