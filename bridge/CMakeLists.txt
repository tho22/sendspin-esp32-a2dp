cmake_minimum_required(VERSION 3.16)

# Bluetooth half of the two-chip setup: I2S in -> A2DP out, controlled over UART (see main/main.cpp)
# sdkconfig.test (optional, git-ignored) overrides single values for A/B tests
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.test")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.test")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sendspin-bt-bridge)
