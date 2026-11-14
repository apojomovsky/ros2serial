# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Cross file for the custom-type example: bare-metal AVR at 16 MHz. The MCU
# defaults to the Uno's ATmega328P; pass -DR2S_MCU=atmega2560 for the Mega, so
# the end-to-end suite builds both boards from one source.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER avr-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
if(NOT R2S_MCU)
  set(R2S_MCU atmega328p)
endif()
add_compile_options(-mmcu=${R2S_MCU} -DF_CPU=16000000UL -Os)
add_link_options(-mmcu=${R2S_MCU})
