cmake_minimum_required(VERSION 3.16)

# Secrets (WiFi) live in a generated, git-ignored file; see build.sh
# sdkconfig.test (optional, git-ignored) overrides single values for A/B tests
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.secrets")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.test")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.test")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sendspin-a2dp)
