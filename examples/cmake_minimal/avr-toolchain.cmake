# Cross file for the cmake example: bare-metal ATmega328P at 16 MHz.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER avr-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
add_compile_options(-mmcu=atmega328p -DF_CPU=16000000UL -Os)
add_link_options(-mmcu=atmega328p)
