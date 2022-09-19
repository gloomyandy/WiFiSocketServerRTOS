cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "src")

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")

if (NOT IDF_TARGET STREQUAL "esp8266")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.defaults.esp32")

    string(REPLACE "esp32" "" chip "${IDF_TARGET}")
    if (chip)
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.${chip}")
    endif()
endif()

project(WiFiSocketServerRTOS)