cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "src")

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")

if (NOT ${IDF_TARGET} STREQUAL "esp8266")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.defaults.idf" "sdkconfig.defaults.${IDF_TARGET}")
endif()

project(WiFiSocketServerRTOS)