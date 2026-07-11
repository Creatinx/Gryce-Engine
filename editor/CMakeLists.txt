# editor/CMakeLists.txt
# Gryce Engine 编辑器入口

add_executable(gryce_engine)

target_sources(gryce_engine PRIVATE
    main.cpp
    ui/debug_panel.cpp
)

target_link_libraries(gryce_engine PRIVATE gryce_core)

# MinGW: 静态链接 libgcc/libstdc++，避免运行时加载到 PATH 中不兼容的运行时 DLL
#（典型症状：无法定位程序输入点 codecvt_utf8_utf16...）。
# 如需动态运行时，在 CMake 配置时传入 -DGRYCE_MINGW_STATIC_RUNTIME=OFF。
if(WIN32 AND MINGW)
    if(NOT DEFINED GRYCE_MINGW_STATIC_RUNTIME)
        set(GRYCE_MINGW_STATIC_RUNTIME ON)
    endif()
    if(GRYCE_MINGW_STATIC_RUNTIME)
        target_link_options(gryce_engine PRIVATE
            -static-libgcc
            -static-libstdc++
            -mconsole
        )
    endif()
endif()

set_target_properties(gryce_engine PROPERTIES
    OUTPUT_NAME gryce
    RUNTIME_OUTPUT_NAME gryce
    FOLDER "Editor"
)

# ---------------------------------------------------------------------------
# 复制 MinGW/UCRT64 运行时 DLL 到输出目录
# 避免运行时加载到 PATH 中其他不兼容的 libstdc++/libgcc 版本
# ---------------------------------------------------------------------------
if(WIN32 AND MINGW)
    get_filename_component(_compiler_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(_runtime_dlls
        ${_compiler_dir}/libgcc_s_seh-1.dll
        ${_compiler_dir}/libwinpthread-1.dll
        ${_compiler_dir}/libstdc++-6.dll
        ${_compiler_dir}/glew32.dll
        ${_compiler_dir}/glfw3.dll
    )
    foreach(_dll IN LISTS _runtime_dlls)
        if(EXISTS ${_dll})
            add_custom_command(TARGET gryce_engine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_dll} $<TARGET_FILE_DIR:gryce_engine>
                COMMENT "Copying runtime DLL: ${_dll}"
            )
        endif()
    endforeach()
endif()
