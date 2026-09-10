# GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
#
# A C++ CLI that packages a game project's res:// content into one or more
# .gpkg archives (GPAK v3, random Base64 store names) through GryceCore's
# PakWriter, then assembles the standalone output directory: template exe +
# runtime DLLs + *.gpkg (no raw res/ copy). Built as part of the normal build
# tree.
#
# Target name is "GryceGC", and the produced executable is also GryceGC.exe
# (PascalCase, matching the documented CLI name). It is a separate CMake target
# from GryceCore: it links the core DLL and uses the exported PakWriter API.
# Entries are split across small per-concern modules; main.cpp only does the
# CLI parsing and dispatch. See comments at the top of main.cpp.
add_executable(GryceGC
    main.cpp
    string_util.cpp
    string_util.h
    random_util.cpp
    random_util.h
    crypto.cpp
    crypto.h
    file_collect.cpp
    file_collect.h
    runtime_util.cpp
    runtime_util.h
    runtime_copy.cpp
    runtime_copy.h
    pack.cpp
    pack.h
    project_skeleton.cpp
    project_skeleton.h
)
set_target_properties(GryceGC PROPERTIES
    RUNTIME_OUTPUT_NAME GryceGC
    FOLDER "Tools"
)
target_include_directories(GryceGC PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(GryceGC PRIVATE GryceCore)
# SHA-256/SHA-512 for the project.data package key (Windows CNG / bcrypt.dll).
if(WIN32)
    target_link_libraries(GryceGC PRIVATE bcrypt)
endif()

# GryceGC 打包游戏时要把 GryceGame 模板 exe 拷入发布目录，二者强耦合。声明
# add_dependencies 保证 GryceGame 先于 GryceGC 构建；模板 exe 的输出目录由根
# CMake 统一设为 ${CMAKE_BINARY_DIR}/bin/<CONFIG>，这里在配置期直接解析出精确
# 路径并转正斜杠（可安全用作字符串字面量）编译进工具，从而丢弃运行时分多个
# 位置猜测查找 GryceGame 的逻辑。仍可用 --game 显式覆盖。
if(TARGET GryceGame)
    add_dependencies(GryceGC GryceGame)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _gryce_cfg)
    get_target_property(_gryce_name GryceGame RUNTIME_OUTPUT_NAME)
    if(NOT _gryce_name)
        set(_gryce_name "GryceGame")
    endif()
    set(_gryce_exe
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_gryce_cfg}}/${_gryce_name}${CMAKE_EXECUTABLE_SUFFIX}")
    file(TO_CMAKE_PATH "${_gryce_exe}" _gryce_exe)
    target_compile_definitions(GryceGC PRIVATE
        GRYCE_GC_GAME_TEMPLATE="${_gryce_exe}"
    )
endif()
