# tests/CMakeLists.txt
# Gryce Engine 单元测试（示例程序已迁移到 examples/）

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------
if(GRYCE_BUILD_TESTS AND GRYCE_HAS_GTEST)
    add_executable(gryce_tests)
    target_sources(gryce_tests
        PRIVATE
            test_main.cpp
            math_test.cpp
            glog_test.cpp
            scene_test.cpp
            asset_test.cpp
            physics_3d_test.cpp
            physics_2d_test.cpp
            fracture_test.cpp
            tilemap_test.cpp
    )
    target_link_libraries(gryce_tests
        PRIVATE
            gryce_core
            GTest::gtest_main
    )
    target_compile_definitions(gryce_tests PRIVATE
        GRYCE_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    target_include_directories(gryce_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(gryce_tests PROPERTIES
        FOLDER "Tests"
    )

    # MinGW: 把编译器自带的 C/C++ 运行时和 GTest DLL 复制到输出目录，
    # 避免运行时因为 PATH 中存在不兼容的 libstdc++/libgcc 版本而崩溃
    #（典型症状：exit=127 或 “无法定位程序输入点 codecvt_utf8_utf16...”）。
    if(WIN32 AND MINGW)
        get_filename_component(_compiler_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
        set(_test_runtime_dlls
            ${_compiler_dir}/libgcc_s_seh-1.dll
            ${_compiler_dir}/libwinpthread-1.dll
            ${_compiler_dir}/libstdc++-6.dll
            ${_compiler_dir}/libgtest.dll
            ${_compiler_dir}/libgtest_main.dll
        )
        foreach(_dll IN LISTS _test_runtime_dlls)
            if(EXISTS ${_dll})
                add_custom_command(TARGET gryce_tests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_dll} $<TARGET_FILE_DIR:gryce_tests>
                    COMMENT "Copying test runtime DLL: ${_dll}"
                )
            endif()
        endforeach()
    endif()

    include(GoogleTest)
    gtest_discover_tests(gryce_tests DISCOVERY_MODE PRE_TEST)
endif()
