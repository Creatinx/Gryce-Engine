# examples/CMakeLists.txt —— Gryce Engine 示例项目（每个子目录是一个完整 GryceGC-A 项目）
#
# parkourDemo：3D 跑酷 demo。玩法逻辑在 QuickJS（game.js）中实现，经 engine.game.*
# 桥驱动 3D，界面由 parkour.uif 声明。

# ---------------------------------------------------------------------------
# 运行时 DLL 拷贝（Windows MinGW）：确保 GLEW/GLFW 随 exe 输出
# ---------------------------------------------------------------------------
function(_gryce_demo_post_build target_name)
    if(WIN32 AND MINGW)
        target_link_options(${target_name} PRIVATE
            -static-libgcc
            -static-libstdc++
        )
        get_filename_component(_compiler_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
        set(_runtime_dlls
            ${_compiler_dir}/glew32.dll
            ${_compiler_dir}/glfw3.dll
            ${_compiler_dir}/libwinpthread-1.dll
        )
        foreach(_dll IN LISTS _runtime_dlls)
            if(EXISTS ${_dll})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_dll} $<TARGET_FILE_DIR:${target_name}>
                    COMMENT "Copying runtime DLL: ${_dll}"
                )
            endif()
        endforeach()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# parkourDemo —— 3D 跑酷（自包含宿主 + 声明式 UI + QuickJS 玩法脚本）
# ---------------------------------------------------------------------------
add_executable(parkourDemo)
target_sources(parkourDemo PRIVATE
    parkourDemo/main.cpp
)
target_link_libraries(parkourDemo PRIVATE GryceEngineUtils)
set_target_properties(parkourDemo PROPERTIES
    FOLDER "Examples"
)
_gryce_demo_post_build(parkourDemo)