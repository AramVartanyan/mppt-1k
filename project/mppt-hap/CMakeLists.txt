# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Repository root: <repo>/components (button, homekit) and <repo>/project/common
# (ads1115, container_nvs, captive-wifi, outputwrite, fupdateota, ...).
if(DEFINED ENV{HOMEKIT_PATH})
  set(HOMEKIT_PATH $ENV{HOMEKIT_PATH})
else()
  set(HOMEKIT_PATH ${CMAKE_CURRENT_LIST_DIR}/../..)
endif(DEFINED ENV{HOMEKIT_PATH})

set(EXTRA_COMPONENT_DIRS
    ${HOMEKIT_PATH}/components
    ${HOMEKIT_PATH}/components/homekit
    ${HOMEKIT_PATH}/project/common)

# Build only "main" and what it REQUIRES (transitively). Other components that
# happen to live in the common folders (e.g. app_wifi with wifi_provisioning)
# are left out of the image.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mppt1hs2)
