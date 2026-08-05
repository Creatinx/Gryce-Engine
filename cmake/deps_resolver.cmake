# cmake/deps_resolver.cmake
# 外部依赖解析：使用 build/deps/ 下由 Python 下载的源码，通过 add_subdirectory 集成。
# 不再使用 FetchContent，避免 CMake 在配置阶段自动下载导致的死板行为。
#
# 如果某个依赖缺失，CMake 配置会报错并提示运行：
#     python tools/deps_manager.py download

set(GRYCE_DEPS_ROOT "${CMAKE_SOURCE_DIR}/build/deps")

# ---------------------------------------------------------------------------
# Helper: check if a dependency is present in build/deps/
# ---------------------------------------------------------------------------
macro(gryce_require_dep name required)
    cmake_parse_arguments(ARG "" "CMAKE_SUBDIR" "" ${ARGN})
    string(TOUPPER "${name}" _name_upper)
    set(_dep_dir "${GRYCE_DEPS_ROOT}/${name}")
    if(ARG_CMAKE_SUBDIR)
        set(_cmake "${_dep_dir}/${ARG_CMAKE_SUBDIR}/CMakeLists.txt")
    else()
        set(_cmake "${_dep_dir}/CMakeLists.txt")
    endif()
    if(EXISTS "${_cmake}")
        set(GRYCE_HAS_${_name_upper} TRUE)
        message(STATUS "[deps] ${name}: found at ${_dep_dir}")
    else()
        set(GRYCE_HAS_${_name_upper} FALSE)
        if(required)
            message(FATAL_ERROR
                "[deps] Required dependency '${name}' not found at ${_dep_dir}\n"
                "Please run: python tools/deps_manager.py download\n"
                "Or manually download the source into ${_dep_dir}")
        else()
            message(STATUS "[deps] ${name}: not found (optional), skipping")
        endif()
    endif()
endmacro()

# ---------------------------------------------------------------------------
# GLFW（窗口系统）
# ---------------------------------------------------------------------------
# MSVC 下优先使用 deps_manager.py 下载的 GLFW 源码构建，避免链接到 MSYS2/MinGW
# 预装的 glfw3（与 MSVC ABI 不兼容，还会把 MinGW 头文件路径带进来）。
gryce_require_dep(glfw TRUE)
if(GRYCE_HAS_GLFW)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    if(MSVC)
        set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
    endif()
    add_subdirectory("${GRYCE_DEPS_ROOT}/glfw" EXCLUDE_FROM_ALL)
    message(STATUS "glfw3: built from source at ${GRYCE_DEPS_ROOT}/glfw")
else()
    find_package(glfw3 QUIET CONFIG)
    if(glfw3_FOUND)
        message(STATUS "glfw3 found via find_package: ${glfw3_DIR}")
        set(GRYCE_HAS_GLFW TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenGL 加载器：GLEW
# ---------------------------------------------------------------------------
# 优先使用 deps_manager.py 下载的 GLEW 源码自行构建，避免链接到 MinGW/MSYS2
# 预装的 GLEW（与 MSVC ABI 不兼容，会导致头文件路径/库格式错乱）。
gryce_require_dep(glew FALSE CMAKE_SUBDIR build/cmake)
if(GRYCE_HAS_GLEW)
    set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GRYCE_DEPS_ROOT}/glew/build/cmake" EXCLUDE_FROM_ALL)
    # GLEW 的 CMakeLists 只导出了 INSTALL_INTERFACE include，
    # 从源码构建时需要补一个 BUILD_INTERFACE，否则消费者找不到 <GL/glew.h>
    if(TARGET glew_s)
        target_include_directories(glew_s PUBLIC $<BUILD_INTERFACE:${GRYCE_DEPS_ROOT}/glew/include>)
    endif()
    if(TARGET glew)
        target_include_directories(glew PUBLIC $<BUILD_INTERFACE:${GRYCE_DEPS_ROOT}/glew/include>)
    endif()
    if(MSVC)
        if(TARGET glew_s)
            target_compile_options(glew_s PRIVATE /w)
        endif()
        if(TARGET glew)
            target_compile_options(glew PRIVATE /w)
        endif()
    endif()
else()
    # 源码缺失时，作为最后手段尝试系统已安装的 GLEW（主要用于非 Windows 平台）
    find_package(GLEW QUIET)
    if(GLEW_FOUND)
        message(STATUS "GLEW found via find_package")
        set(GRYCE_HAS_GLEW TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# 数学库：glm（可选，引擎自研 math.h 不依赖它）
# ---------------------------------------------------------------------------
find_package(glm QUIET CONFIG)
if(glm_FOUND)
    message(STATUS "glm found: ${glm_DIR}")
    set(GRYCE_HAS_GLM TRUE)
else()
    message(STATUS "glm not found locally, skipping")
    set(GRYCE_HAS_GLM FALSE)
endif()

# ---------------------------------------------------------------------------
# 日志库：spdlog（可选，引擎自研 Logger 不依赖它）
# ---------------------------------------------------------------------------
find_package(spdlog QUIET CONFIG)
if(spdlog_FOUND)
    message(STATUS "spdlog found: ${spdlog_DIR}")
    set(GRYCE_HAS_SPDLOG TRUE)
else()
    message(STATUS "spdlog not found locally, skipping")
    set(GRYCE_HAS_SPDLOG FALSE)
endif()

# ---------------------------------------------------------------------------
# JSON：nlohmann/json（场景序列化 .gesc）— 捆绑头文件版本
# ---------------------------------------------------------------------------
find_package(nlohmann_json QUIET CONFIG)
if(nlohmann_json_FOUND)
    message(STATUS "nlohmann_json found via find_package: ${nlohmann_json_DIR}")
    set(GRYCE_HAS_NLOHMANN_JSON TRUE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/nlohmann/json.hpp")
    message(STATUS "nlohmann_json found in third_party, using bundled header-only copy")
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json>
    )
    set(GRYCE_HAS_NLOHMANN_JSON TRUE)
else()
    message(FATAL_ERROR
        "nlohmann/json not found. Expected bundled copy at third_party/nlohmann_json/\n"
        "Please ensure the repository is cloned correctly.")
endif()

# ---------------------------------------------------------------------------
# 模型导入：Assimp（仅启用 Blender 常用格式，可选）
# ---------------------------------------------------------------------------
option(GRYCE_FETCH_ASSIMP "Enable Assimp model importer" ON)
find_package(assimp QUIET CONFIG)
if(assimp_FOUND)
    message(STATUS "assimp found via find_package: ${assimp_DIR}")
    set(GRYCE_HAS_ASSIMP TRUE)
elseif(GRYCE_FETCH_ASSIMP)
    gryce_require_dep(assimp TRUE)
    if(GRYCE_HAS_ASSIMP)
        set(ASSIMP_BUILD_TESTS              OFF CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_ASSIMP_TOOLS       OFF CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_SAMPLES            OFF CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_DOCS               OFF CACHE BOOL "" FORCE)
        set(ASSIMP_INSTALL                  OFF CACHE BOOL "" FORCE)
        set(ASSIMP_NO_EXPORT                ON  CACHE BOOL "" FORCE)
        set(ASSIMP_WARNINGS_AS_ERRORS       OFF CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_OBJ_IMPORTER       ON CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_FBX_IMPORTER       ON CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_GLTF_IMPORTER      ON CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_COLLADA_IMPORTER   ON CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_PLY_IMPORTER       ON CACHE BOOL "" FORCE)
        set(ASSIMP_BUILD_STL_IMPORTER       ON CACHE BOOL "" FORCE)
        add_subdirectory("${GRYCE_DEPS_ROOT}/assimp" EXCLUDE_FROM_ALL)
        set(GRYCE_HAS_ASSIMP TRUE)
    endif()
else()
    message(STATUS "assimp disabled (GRYCE_FETCH_ASSIMP=OFF); built-in OBJ loader will be used")
    set(GRYCE_HAS_ASSIMP FALSE)
endif()

# ---------------------------------------------------------------------------
# UI：Dear ImGui（docking 分支）— 捆绑源码
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.h")
    message(STATUS "imgui found in third_party/imgui, using bundled copy")
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "" FORCE)
    set(GRYCE_HAS_IMGUI TRUE)
else()
    message(FATAL_ERROR
        "Dear ImGui not found. Expected bundled copy at third_party/imgui/\n"
        "Please ensure the repository is cloned correctly.")
endif()

# ---------------------------------------------------------------------------
# 2D 物理：Box2D（可选）
# ---------------------------------------------------------------------------
option(GRYCE_FETCH_BOX2D "Enable Box2D 2D physics" ON)
find_package(box2d QUIET CONFIG)
if(box2d_FOUND)
    message(STATUS "box2d found via find_package: ${box2d_DIR}")
    set(GRYCE_HAS_BOX2D TRUE)
elseif(GRYCE_FETCH_BOX2D)
    gryce_require_dep(box2d FALSE)
    if(GRYCE_HAS_BOX2D)
        set(BOX2D_BUILD_TESTBED    OFF CACHE BOOL "" FORCE)
        set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BOX2D_INSTALL          OFF CACHE BOOL "" FORCE)
        set(BOX2D_SAMPLES          OFF CACHE BOOL "" FORCE)
        add_subdirectory("${GRYCE_DEPS_ROOT}/box2d" EXCLUDE_FROM_ALL)
        if(TARGET box2d)
            if(MSVC)
                target_compile_options(box2d PRIVATE /w)
            else()
                target_compile_options(box2d PRIVATE -w)
            endif()
        endif()
        set(GRYCE_HAS_BOX2D TRUE)
    endif()
else()
    message(STATUS "box2d disabled (GRYCE_FETCH_BOX2D=OFF); 2D physics disabled")
    set(GRYCE_HAS_BOX2D FALSE)
endif()

# ---------------------------------------------------------------------------
# 3D 物理：JoltPhysics（可选，主后端）
#   CMakeLists.txt 位于 Build/ 子目录下
# ---------------------------------------------------------------------------
option(GRYCE_FETCH_JOLT "Enable JoltPhysics 3D physics" ON)
find_package(Jolt QUIET CONFIG)
if(Jolt_FOUND)
    message(STATUS "Jolt found via find_package: ${Jolt_DIR}")
    set(GRYCE_HAS_JOLT TRUE)
elseif(GRYCE_FETCH_JOLT)
    gryce_require_dep(jolt FALSE CMAKE_SUBDIR Build)
    if(GRYCE_HAS_JOLT)
        set(USE_AVX OFF CACHE BOOL "" FORCE)
        set(USE_AVX2 OFF CACHE BOOL "" FORCE)
        set(USE_SSE4_1 ON CACHE BOOL "" FORCE)
        set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
        set(USE_LZCNT ON CACHE BOOL "" FORCE)
        set(USE_TZCNT ON CACHE BOOL "" FORCE)
        set(USE_F16C ON CACHE BOOL "" FORCE)
        set(USE_FMADD ON CACHE BOOL "" FORCE)
        set(JPH_INSTALL_RUNTIME OFF CACHE BOOL "" FORCE)
        set(JPH_INSTALL_DEVELOPMENT OFF CACHE BOOL "" FORCE)
        # Jolt 默认使用静态 MSVC 运行时 (/MT)，与 Gryce 默认的动态运行时 (/MD) 冲突
        set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
        add_subdirectory("${GRYCE_DEPS_ROOT}/jolt/Build" EXCLUDE_FROM_ALL)
        if(MSVC AND TARGET Jolt)
            # Jolt 默认 /Wall /WX，在 VS2026 + 新 Windows SDK 下会把 SDK 头里的
            # C4865 警告当成错误；关闭 warnings-as-errors 并禁用迁移警告。
            target_compile_options(Jolt PRIVATE /WX- /Wv:18)
        endif()
        set(GRYCE_HAS_JOLT TRUE)
    endif()
else()
    message(STATUS "Jolt disabled (GRYCE_FETCH_JOLT=OFF); 3D physics fallback")
    set(GRYCE_HAS_JOLT FALSE)
endif()

# ---------------------------------------------------------------------------
# 测试框架：GoogleTest（可选）
# MSVC 下优先使用 deps_manager.py 下载的源码构建，避免链接到 MSYS2 预装的
# GTest（其 include 目录指向 MinGW 头文件，与 MSVC 不兼容）。
# ---------------------------------------------------------------------------
if(GRYCE_BUILD_TESTS)
    # MSVC 优先本地源码；其他平台保留先 find_package 的习惯
    if(MSVC)
        gryce_require_dep(googletest FALSE)
        if(GRYCE_HAS_GOOGLETEST)
            set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            add_subdirectory("${GRYCE_DEPS_ROOT}/googletest" EXCLUDE_FROM_ALL)
            # add_subdirectory 后生成 gtest / gtest_main 目标，但测试目标用的是 GTest::gtest_main
            if(TARGET gtest_main AND NOT TARGET GTest::gtest_main)
                add_library(GTest::gtest_main ALIAS gtest_main)
            endif()
            if(TARGET gtest AND NOT TARGET GTest::gtest)
                add_library(GTest::gtest ALIAS gtest)
            endif()
            message(STATUS "GTest: built from source at ${GRYCE_DEPS_ROOT}/googletest")
            set(GRYCE_HAS_GTEST TRUE)
        else()
            # 显式报错而非静默禁用：GRYCE_BUILD_TESTS=ON 却跑不了测试会让人误以为
            # 测试通过了。提示用 deps_manager 下载 googletest 源码。
            message(FATAL_ERROR
                "[deps] GRYCE_BUILD_TESTS=ON but GoogleTest source not found at "
                "${GRYCE_DEPS_ROOT}/googletest\n"
                "Please run: python tools/deps_manager.py download\n"
                "Or pass -DGRYCE_BUILD_TESTS=OFF to disable tests."
            )
        endif()
    else()
        find_package(GTest QUIET CONFIG)
        if(GTest_FOUND)
            message(STATUS "GTest found: ${GTest_DIR}")
            set(GRYCE_HAS_GTEST TRUE)
        else()
            message(FATAL_ERROR
                "[deps] GRYCE_BUILD_TESTS=ON but GoogleTest not found.\n"
                "Please run: python tools/deps_manager.py download\n"
                "Or pass -DGRYCE_BUILD_TESTS=OFF to disable tests."
            )
        endif()
    endif()
endif()
