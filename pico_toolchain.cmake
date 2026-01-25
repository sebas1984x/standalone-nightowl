set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m0plus)

# Use the ARM bare-metal toolchain
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

# Optional: make output smaller / more embedded-friendly
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
