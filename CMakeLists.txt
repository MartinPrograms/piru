cmake_minimum_required(VERSION 4.3)
project(PIRU)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(COMPILE_FLAGS
        -O0
        -ffreestanding
        -fno-asynchronous-unwind-tables
        -fno-stack-protector
        -Wall -Wextra -Werror -Wpedantic
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-function
        -mcmodel=kernel
        -mno-red-zone
        -m64
        -mabi=sysv
        -mno-sse2
        -mno-mmx
        -mno-avx
        -mno-avx512f
)

set(LINKER_OPTIONS
        -mcmodel=kernel
        -mno-red-zone
        -m64
        -Wl,--hash-style=sysv
        -mabi=sysv
)

set(COMPILE_FLAGS_CXX
        ${COMPILE_FLAGS}
        -fno-exceptions
        -fno-rtti
        -fno-common
        -Wno-vla
        -Wno-zero-length-array
        -Wno-gnu-flexible-array-member
        -fno-use-cxa-atexit
)

set(COMPILE_FLAGS_C
        ${COMPILE_FLAGS}
        -fno-exceptions
        -Wno-vla
        -Wno-zero-length-array
)

set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_C_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_C_IMPLICIT_LINK_DIRECTORIES "")

add_subdirectory(src)

add_custom_target(CopyKernel ALL
        DEPENDS piru-kernel.elf
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:piru-kernel.elf> ${CMAKE_SOURCE_DIR}/limine-config/boot/piru-kernel.elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Piru is the target project, it will contain the drivers and userspace programs too
add_custom_target(piru ALL DEPENDS CopyKernel)