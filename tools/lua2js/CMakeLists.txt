# GryceLua2JS — Lua 到 JS 迁移辅助工具。
#
# lua2js 静态库供单元测试链接；GryceLua2JS 为 CLI 可执行程序。
add_library(lua2js STATIC lua2js.cpp)
target_include_directories(lua2js PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lua2js PROPERTIES FOLDER "Tools")

add_executable(GryceLua2JS main.cpp)
target_link_libraries(GryceLua2JS PRIVATE lua2js)
set_target_properties(GryceLua2JS PROPERTIES
    RUNTIME_OUTPUT_NAME lua2js
    FOLDER "Tools"
)
