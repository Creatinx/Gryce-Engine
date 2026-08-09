# Lua 5.4.7 (official sources, Lua.org / PUC-Rio) vendored for GryceSRT.
# Static library linked into GryceCore; the CLI tools (lua.c/luac.c) are
# intentionally excluded.
add_library(GryceLua STATIC
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)

target_include_directories(GryceLua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(GryceLua PRIVATE /W3)
else()
    target_compile_options(GryceLua PRIVATE -Wall -Wextra)
endif()
