# GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
#
# A C++ CLI that packages a game project's res:// content into one or more
# .gpkg archives (GPAK v3, random Base64 store names) through GryceCore's
# PakWriter, then assembles the standalone output directory: template exe +
# runtime DLLs + *.gpkg (no raw res/ copy). Built as part of the normal build
# tree.
#
# Target name is "GryceGC" (shown in Visual Studio); the executable itself is
# grycegc.exe. It is a separate CMake target from GryceCore: it links the core
# DLL and uses the exported PakWriter runtime API.
add_executable(GryceGC main.cpp)
set_target_properties(GryceGC PROPERTIES
    RUNTIME_OUTPUT_NAME grycegc
    FOLDER "Tools"
)
target_include_directories(GryceGC PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(GryceGC PRIVATE GryceCore)
# SHA-256/SHA-512 for the gdata package key (Windows CNG / bcrypt.dll).
if(WIN32)
    target_link_libraries(GryceGC PRIVATE bcrypt)
endif()
