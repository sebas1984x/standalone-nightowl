# Project-local Pico SDK import wrapper

if (DEFINED ENV{PICO_SDK_PATH})
  set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if (NOT PICO_SDK_PATH)
  message(FATAL_ERROR "PICO_SDK_PATH is not set")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
