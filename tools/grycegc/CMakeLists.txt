# GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
#
# A C++ CLI that packages a game project's res:// content into one or more
# .gpkg archives (GPAK format) through the GryceCore GPack C API, then
# assembles the standalone output directory: template exe + runtime DLLs +
# *.gpkg (no raw res/ copy). Built as part of the normal build tree.
add_executable(grycegc main.cpp)
set_target_properties(grycegc PROPERTIES
    FOLDER "Tools"
)
target_include_directories(grycegc PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(grycegc PRIVATE GryceCore)
