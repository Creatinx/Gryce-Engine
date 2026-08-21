# editor_deploy.cmake — 把原生 DLL 部署到 WPF 编辑器输出目录。
#
# 与直接使用 cmake -E copy_if_different 的区别：
# 编辑器正在运行时 DLL/exe 会被锁定，直接复制会让整个构建失败。
# 这里失败只打警告、下次构建继续重试，保证 MinGW/MSVC 开发循环都顺畅，
# 同时保留"每次手动复制链接库都要在 CMake 里自动复制"的规则。
#
# 用法：
#   cmake -DGRYCE_EDITOR_BIN=<输出目录>
#         -DGRYCE_SRC=<源 DLL>
#         -DGRYCE_NAME=<目标文件名>
#         -P cmake/editor_deploy.cmake

if(EXISTS "${GRYCE_SRC}")
    file(MAKE_DIRECTORY "${GRYCE_EDITOR_BIN}")
    set(_dst "${GRYCE_EDITOR_BIN}/${GRYCE_NAME}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GRYCE_SRC}" "${_dst}"
        RESULT_VARIABLE _deploy_result
    )
    if(NOT _deploy_result EQUAL 0)
        message(WARNING
            "editor_deploy: cannot deploy ${GRYCE_NAME} to ${_dst} "
            "(editor running? file locked). Will retry on next build.")
    endif()
else()
    message(WARNING "editor_deploy: source not found: ${GRYCE_SRC}")
endif()
