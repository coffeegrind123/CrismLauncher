cmake_minimum_required(VERSION 3.15)
project(quickjs LANGUAGES C)

# quickjs-ng v0.18.0, the release's quickjs-amalgam.zip (sha256 6c05066a6b62b483e2ba0f83d7a16688c6958182b214bfd693529434cef84a9e).
# Only the engine: QJS_BUILD_LIBC stays undefined, so the std/os modules (files, processes, sockets) are not compiled in.
add_library(quickjs STATIC src/quickjs-amalgam.c include/quickjs.h)
target_include_directories(quickjs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(quickjs PRIVATE QUICKJS_NG_BUILD)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON C_VISIBILITY_PRESET hidden)

# Upstream's own flag set: third-party code, so its warnings are silenced rather than fixed
if(MSVC)
    target_compile_options(quickjs PRIVATE /experimental:c11atomics /W0)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(quickjs PRIVATE -w)
    endif()
else()
    target_compile_options(quickjs PRIVATE -w -funsigned-char)
endif()
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
    target_compile_definitions(quickjs PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
endif()

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(quickjs PRIVATE Threads::Threads)
endif()
if(UNIX)
    target_link_libraries(quickjs PRIVATE m)
endif()
