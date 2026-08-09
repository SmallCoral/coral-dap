cmake_minimum_required(VERSION 3.22)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m3)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(f103_blinky LANGUAGES C ASM)

set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32F103CB_FLASH.ld")

add_executable(f103_blinky
  main.c
  startup_stm32f103cb.s
)

target_compile_options(f103_blinky PRIVATE
  -mcpu=cortex-m3
  -mthumb
  -Os
  -ffreestanding
  -fdata-sections
  -ffunction-sections
  -Wall
  -Wextra
  -Werror
)

target_link_options(f103_blinky PRIVATE
  -mcpu=cortex-m3
  -mthumb
  -nostdlib
  "-T${LINKER_SCRIPT}"
  -Wl,--gc-sections
  -Wl,--print-memory-usage
  "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/f103_blinky.map"
)

set_target_properties(f103_blinky PROPERTIES SUFFIX ".elf")

find_program(ARM_OBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(ARM_SIZE arm-none-eabi-size REQUIRED)

add_custom_command(TARGET f103_blinky POST_BUILD
  COMMAND "${ARM_OBJCOPY}" -O ihex
          "$<TARGET_FILE:f103_blinky>"
          "${CMAKE_CURRENT_BINARY_DIR}/f103_blinky.hex"
  COMMAND "${ARM_OBJCOPY}" -O binary
          "$<TARGET_FILE:f103_blinky>"
          "${CMAKE_CURRENT_BINARY_DIR}/f103_blinky.bin"
  COMMAND "${ARM_SIZE}" "$<TARGET_FILE:f103_blinky>"
  VERBATIM
)
