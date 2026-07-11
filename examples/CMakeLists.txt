# examples/CMakeLists.txt
# Gryce Engine 示例游戏项目（每个子目录都是一个完整的 GryceGC-A 项目）

set(EXAMPLES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ---------------------------------------------------------------------------
# 3D 示例项目
# ---------------------------------------------------------------------------
add_executable(3dtest)
target_sources(3dtest PRIVATE
    3dtest/3dtest.cpp
    ${EXAMPLES_COMMON_DIR}/ui/debug_panel.cpp
)
target_link_libraries(3dtest PRIVATE gryce_core)
target_include_directories(3dtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXAMPLES_COMMON_DIR}
)
set_target_properties(3dtest PROPERTIES
    FOLDER "Examples"
)

# ---------------------------------------------------------------------------
# 2D 平台示例项目
# ---------------------------------------------------------------------------
add_executable(gt2dDemo)
target_sources(gt2dDemo PRIVATE
    gt2dDemo/gt2dDemo.cpp
    ${EXAMPLES_COMMON_DIR}/ui/debug_panel.cpp
)
target_link_libraries(gt2dDemo PRIVATE gryce_core)
target_include_directories(gt2dDemo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXAMPLES_COMMON_DIR}
)
set_target_properties(gt2dDemo PROPERTIES
    FOLDER "Examples"
)

# ---------------------------------------------------------------------------
# MinGW: 静态链接 C++ 运行时，避免 libstdc++/libgcc DLL 版本不匹配导致
# "无法定位程序输入点 codecvt_utf8_utf16..."
# ---------------------------------------------------------------------------
if(WIN32 AND MINGW)
    foreach(_target IN ITEMS 3dtest gt2dDemo)
        target_link_options(${_target} PRIVATE
            -static-libgcc
            -static-libstdc++
        )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# MinGW: 仅复制第三方 DLL（GLFW/GLEW），不再复制运行时库
# ---------------------------------------------------------------------------
if(WIN32 AND MINGW)
    get_filename_component(_compiler_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(_runtime_dlls
        ${_compiler_dir}/glew32.dll
        ${_compiler_dir}/glfw3.dll
    )
    foreach(_target IN ITEMS 3dtest gt2dDemo)
        foreach(_dll IN LISTS _runtime_dlls)
            if(EXISTS ${_dll})
                add_custom_command(TARGET ${_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_dll} $<TARGET_FILE_DIR:${_target}>
                    COMMENT "Copying runtime DLL: ${_dll}"
                )
            endif()
        endforeach()
    endforeach()
endif()
