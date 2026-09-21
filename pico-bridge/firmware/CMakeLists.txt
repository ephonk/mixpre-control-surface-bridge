cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(mixpre_bridge C CXX ASM)

# The version lives in version.h and ends up in the name of the .uf2 file, so the
# file on disk always says which build it is.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/version.h FW_VERSION_LINE REGEX "#define FW_VERSION ")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" FW_VERSION "${FW_VERSION_LINE}")
pico_sdk_init()

# Pico-PIO-USB library (low-level PIO USB, used as host controller via TinyUSB)
set(PICO_PIO_USB_DIR ${CMAKE_CURRENT_LIST_DIR}/../Pico-PIO-USB)
add_subdirectory(${PICO_PIO_USB_DIR} pico_pio_usb)

add_executable(mixpre_bridge
  main.c
  usb_descriptors.c
  midi_host.c
  korg_emu.c
  dbglog.c
  # TinyUSB PIO-USB host controller driver.
  # Native USB = device (spoof Korg), PIO USB = host (Icon).
  ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

target_compile_definitions(mixpre_bridge PRIVATE
  PIO_USB_USE_TINYUSB
)

target_include_directories(mixpre_bridge PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(mixpre_bridge PRIVATE
  pico_stdlib
  hardware_pwm
  hardware_gpio
  pico_multicore
  pico_flash
  hardware_flash
  pico_pio_usb
  tinyusb_device
  tinyusb_host
)

set_target_properties(mixpre_bridge PROPERTIES OUTPUT_NAME "mixpre_bridge_${FW_VERSION}")
pico_add_extra_outputs(mixpre_bridge)
