# GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
#
# A C++ CLI that packages a game project's res:// content into one or more
# .gpkg archives (GPAK format) through the GryceCore GPack C API, then
# assembles the standalone output directory: template exe + runtime DLLs +
# *.gpkg (no raw res/ copy). Built as part of the normal build tree.
#
# Target name is "GryceGC" (shown in Visual Studio); the executable itself is
# grycegc.exe. It is a separate CMake target from GryceCore: it links the core
# DLL through its import library and uses the exported GPack C API only.
add_executable(GryceGC main.cpp)
set_target_properties(GryceGC PROPERTIES
    RUNTIME_OUTPUT_NAME grycegc
    FOLDER "Tools"
)
target_include_directories(GryceGC PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(GryceGC PRIVATE GryceCore)
