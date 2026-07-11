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

    include(GoogleTest)
    gtest_discover_tests(gryce_tests DISCOVERY_MODE PRE_TEST)
endif()
