# cmake/dependencies.cmake
# 第三方依赖：仅本地查找，找不到才下载

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# 缓存目录支持：build.py 预下载的依赖包优先使用本地文件
# ---------------------------------------------------------------------------
if(DEFINED GRYCE_CACHE_DIR AND EXISTS "${GRYCE_CACHE_DIR}")
    set(_GRYCE_HAS_CACHE TRUE)
    message(STATUS "[Gryce Engine] Using dependency cache: ${GRYCE_CACHE_DIR}")
else()
    set(_GRYCE_HAS_CACHE FALSE)
endif()

# ---------------------------------------------------------------------------
# GLFW（窗口系统）
# ---------------------------------------------------------------------------
find_package(glfw3 QUIET CONFIG)
if(glfw3_FOUND)
    message(STATUS "glfw3 found: ${glfw3_DIR}")
    set(GRYCE_HAS_GLFW TRUE)
else()
    if(_GRYCE_HAS_CACHE AND EXISTS "${GRYCE_CACHE_DIR}/glfw-3.4.tar.gz")
        set(GLFW_URL "file://${GRYCE_CACHE_DIR}/glfw-3.4.tar.gz")
        message(STATUS "Using cached glfw from ${GRYCE_CACHE_DIR}")
    else()
        set(GLFW_URL "https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz")
    endif()
    message(STATUS "glfw3 not found locally, fetching from ${GLFW_URL}...")
    FetchContent_Declare(
        glfw
        URL       ${GLFW_URL}
        DOWNLOAD_NO_PROGRESS FALSE
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    set(GRYCE_HAS_GLFW TRUE)
endif()

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# GLFW（窗口系统）
# ---------------------------------------------------------------------------
find_package(glfw3 QUIET CONFIG)
if(glfw3_FOUND)
    message(STATUS "glfw3 found: ${glfw3_DIR}")
    set(GRYCE_HAS_GLFW TRUE)
else()
    message(STATUS "glfw3 not found locally, fetching from GitHub...")
    FetchContent_Declare(
        glfw
        URL       https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
        DOWNLOAD_NO_PROGRESS FALSE
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    set(GRYCE_HAS_GLFW TRUE)
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
# JSON：nlohmann/json（场景序列化 .gesc）
# ---------------------------------------------------------------------------
find_package(nlohmann_json QUIET CONFIG)
if(nlohmann_json_FOUND)
    message(STATUS "nlohmann_json found: ${nlohmann_json_DIR}")
    set(GRYCE_HAS_NLOHMANN_JSON TRUE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/nlohmann/json.hpp")
    message(STATUS "nlohmann_json found in third_party, using bundled header-only copy")
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json>
    )
    set(GRYCE_HAS_NLOHMANN_JSON TRUE)
elseif(EXISTS "${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include/nlohmann/json.hpp")
    message(STATUS "nlohmann_json source found in _deps, using existing copy")
    FetchContent_Declare(
        nlohmann_json
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src
    )
    FetchContent_MakeAvailable(nlohmann_json)
    set(GRYCE_HAS_NLOHMANN_JSON TRUE)
else()
    message(STATUS "nlohmann_json not found locally, fetching from GitHub...")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
    set(GRYCE_HAS_NLOHMANN_JSON TRUE)
endif()

# ---------------------------------------------------------------------------
# 模型导入：Assimp（仅启用 Blender 常用格式，可选）
# ---------------------------------------------------------------------------
option(GRYCE_FETCH_ASSIMP "Fetch Assimp from GitHub (requires network)" ON)
find_package(assimp QUIET CONFIG)
if(assimp_FOUND)
    message(STATUS "assimp found: ${assimp_DIR}")
    set(GRYCE_HAS_ASSIMP TRUE)
elseif(GRYCE_FETCH_ASSIMP)
    if(_GRYCE_HAS_CACHE AND EXISTS "${GRYCE_CACHE_DIR}/assimp-v5.4.3.tar.gz")
        set(ASSIMP_URL "file://${GRYCE_CACHE_DIR}/assimp-v5.4.3.tar.gz")
        message(STATUS "Using cached assimp from ${GRYCE_CACHE_DIR}")
    else()
        set(ASSIMP_URL "https://github.com/assimp/assimp/archive/refs/tags/v5.4.3.tar.gz")
    endif()
    message(STATUS "assimp not found locally, fetching from ${ASSIMP_URL}...")
    FetchContent_Declare(
        assimp
        URL       ${ASSIMP_URL}
        URL_HASH  SHA256=9cdd1fb0a778618506dd89c0d850667ec1312e05453ef569e19b463ca1abded2
        DOWNLOAD_NO_PROGRESS FALSE
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(ASSIMP_BUILD_TESTS              OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS       OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES            OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_DOCS               OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL                  OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT                ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    # 仅启用 Blender 常用导出格式
    set(ASSIMP_BUILD_OBJ_IMPORTER       ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_FBX_IMPORTER       ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER      ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_COLLADA_IMPORTER   ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_PLY_IMPORTER       ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_STL_IMPORTER       ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(assimp)
    set(GRYCE_HAS_ASSIMP TRUE)
else()
    message(STATUS "assimp not found locally and GRYCE_FETCH_ASSIMP=OFF; built-in OBJ loader will be used")
    set(GRYCE_HAS_ASSIMP FALSE)
endif()

# ---------------------------------------------------------------------------
# UI：Dear ImGui（docking 分支）
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.h")
    message(STATUS "imgui found in third_party/imgui, using bundled docking copy")
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "" FORCE)
    set(GRYCE_HAS_IMGUI TRUE)
else()
    find_package(imgui QUIET CONFIG)
    if(imgui_FOUND)
        message(STATUS "imgui found: ${imgui_DIR}")
        set(GRYCE_HAS_IMGUI TRUE)
    else()
        message(STATUS "imgui not found locally, fetching docking branch from GitHub...")
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        docking
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(imgui)
        set(IMGUI_DIR "${imgui_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(GRYCE_HAS_IMGUI TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# 2D 物理：Box2D
# ---------------------------------------------------------------------------
option(GRYCE_FETCH_BOX2D "Fetch Box2D from GitHub (requires network)" ON)
find_package(box2d QUIET CONFIG)
if(box2d_FOUND)
    message(STATUS "box2d found: ${box2d_DIR}")
    set(GRYCE_HAS_BOX2D TRUE)
elseif(GRYCE_FETCH_BOX2D)
    message(STATUS "box2d not found locally, fetching from GitHub...")
    FetchContent_Declare(
        box2d
        GIT_REPOSITORY https://github.com/erincatto/box2d.git
        GIT_TAG        v3.0.0
        GIT_SHALLOW    TRUE
    )
    set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_INSTALL OFF CACHE BOOL "" FORCE)
    set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(box2d)
    if(TARGET box2d)
        target_compile_options(box2d PRIVATE -w)
    endif()
    set(GRYCE_HAS_BOX2D TRUE)
else()
    message(STATUS "box2d not found locally and GRYCE_FETCH_BOX2D=OFF; 2D physics disabled")
    set(GRYCE_HAS_BOX2D FALSE)
endif()

# ---------------------------------------------------------------------------
# 3D 物理：JoltPhysics（主后端）
# 默认不自动抓取：Jolt 仓库较大，网络不稳定时容易失败。
# 需要时打开 -DGRYCE_FETCH_JOLT=ON，或本地安装 Jolt 后通过 find_package 使用。
# ---------------------------------------------------------------------------
option(GRYCE_FETCH_JOLT "Fetch JoltPhysics from GitHub (requires network)" OFF)
find_package(Jolt QUIET CONFIG)
if(Jolt_FOUND)
    message(STATUS "Jolt found: ${Jolt_DIR}")
    set(GRYCE_HAS_JOLT TRUE)
elseif(GRYCE_FETCH_JOLT)
    message(STATUS "Jolt not found locally, fetching from GitHub...")
    FetchContent_Declare(
        JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG        v5.2.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  Build
    )
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
    FetchContent_MakeAvailable(JoltPhysics)
    set(GRYCE_HAS_JOLT TRUE)
else()
    message(STATUS "Jolt not fetched; 3D physics will use builtin fallback (set GRYCE_FETCH_JOLT=ON to download)")
    set(GRYCE_HAS_JOLT FALSE)
endif()

# ---------------------------------------------------------------------------
# 测试框架：GoogleTest（可选，找不到则自动关闭 GRYCE_BUILD_TESTS）
# ---------------------------------------------------------------------------
if(GRYCE_BUILD_TESTS)
    find_package(GTest QUIET CONFIG)
    if(GTest_FOUND)
        message(STATUS "GTest found: ${GTest_DIR}")
        set(GRYCE_HAS_GTEST TRUE)
    else()
        message(STATUS "GTest not found locally, disabling tests")
        set(GRYCE_HAS_GTEST FALSE)
        set(GRYCE_BUILD_TESTS OFF)
    endif()
endif()
