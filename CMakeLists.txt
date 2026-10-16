cmake_minimum_required(VERSION 3.16)
# esp-homekit-sdk (sous-module fige) : seulement ses composants HomeKit, ni examples/ ni button.
# esp_hap_extras (OTA HTTPS du SDK, BCT) n'est utilise par aucun autre composant : exclu.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit")
set(EXCLUDE_COMPONENTS esp_hap_extras)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(interphone-homekit)
