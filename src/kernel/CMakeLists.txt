project(piru-kernel LANGUAGES C CXX ASM)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.cpp" "*.S" "*.c" "*.hpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "*.hpp" "*.h")

# External dependencies
include (FetchContent)

FetchContent_Declare(
        flanterm
        GIT_REPOSITORY https://github.com/Mintsuki/Flanterm.git
        GIT_TAG        trunk
)

FetchContent_Declare(
        freestanding_meta
        GIT_REPOSITORY https://github.com/MartinPrograms/freestanding_meta.git
        GIT_TAG master
)

FetchContent_MakeAvailable(flanterm freestanding_meta)

file(GLOB_RECURSE FLANTERM_SOURCES CONFIGURE_DEPENDS ${flanterm_SOURCE_DIR}/src/*.c)

# The piru-kernel.api are all the definitions for everything outside of piru-kernel, like drivers
add_library(piru-kernel.api INTERFACE)
target_include_directories(piru-kernel.api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include ${flanterm_SOURCE_DIR}/src)

file(GLOB_RECURSE FLANTERM_SOURCES CONFIGURE_DEPENDS ${flanterm_SOURCE_DIR}/src/*.c)

# .elf file
add_executable(piru-kernel.elf ${SOURCES} ${HEADERS} ${FLANTERM_SOURCES})
target_link_libraries(piru-kernel.elf PRIVATE piru-kernel.api freestanding_meta)

include(Subsystems.cmake)
include(Options.cmake)

target_include_directories(piru-kernel.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/piru)

target_compile_options(piru-kernel.elf PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${COMPILE_FLAGS_CXX} -fno-threadsafe-statics -freflection>
        $<$<COMPILE_LANGUAGE:C>:${COMPILE_FLAGS_C}>
)

target_link_options(piru-kernel.elf PRIVATE
        -g
        -T ${CMAKE_SOURCE_DIR}/limine-config/linker.ld
        -nostdlib
        -fvisibility=default
        -ggdb
        -nostartfiles
        ${LINKER_OPTIONS}
)