# QuickJS — Embeddable Javascript Engine (MIT License)
# https://bellard.org/quickjs/
# Version: 2026-06-04
#
# Static library for embedding into GryceEngine UI script system.
# Only core engine sources are compiled; the CLI tools (qjs.c, qjsc.c) and
# test files are excluded.

add_library(GryceQuickJS STATIC
    quickjs.c
    quickjs-libc.c
    cutils.c
    dtoa.c
    libregexp.c
    libunicode.c
)

target_include_directories(GryceQuickJS PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Define CONFIG_VERSION (same as upstream Makefile: -DCONFIG_VERSION=\"$(cat VERSION)\")
target_compile_definitions(GryceQuickJS PRIVATE
    CONFIG_VERSION="2026-06-04"
)

# On MinGW, QuickJS needs _GNU_SOURCE and __USE_MINGW_ANSI_STDIO
if(WIN32)
    target_compile_definitions(GryceQuickJS PRIVATE
        _WIN32
        WIN32_LEAN_AND_MEAN
        _GNU_SOURCE
        __USE_MINGW_ANSI_STDIO
    )
endif()

# Suppress warnings from QuickJS C code (it's a C library, not our code)
if(MSVC)
    target_compile_options(GryceQuickJS PRIVATE /W0)
else()
    target_compile_options(GryceQuickJS PRIVATE -w)
endif()

set_target_properties(GryceQuickJS PROPERTIES
    FOLDER "ThirdParty"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)