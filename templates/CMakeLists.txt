# GryceSPC game template: a standalone executable that links the core libraries
# and runs a scene with scripts (built/packaged by GryceGC).
if(GRYCE_BUILD_GAME_TEMPLATE)
    add_executable(GryceGame game_main.cpp)
    set_target_properties(GryceGame PROPERTIES
        RUNTIME_OUTPUT_NAME GryceGame
        FOLDER "Game"
    )
    target_include_directories(GryceGame PRIVATE ${CMAKE_SOURCE_DIR}/core)
    target_link_libraries(GryceGame PRIVATE
        GryceCore
        GryceRenderer
        GrycePlatform
        GrycePhysics
    )
    # GryceGC packages the runtime DLLs into a "runtime/" subfolder next to
    # the exe; delay-load them so the game entry can extend the DLL search
    # path (SetDllDirectoryW("runtime")) before the first engine call.
    if(MSVC)
        target_link_options(GryceGame PRIVATE
            $<$<CONFIG:Debug>:
                /DELAYLOAD:GryceCored.dll
                /DELAYLOAD:GryceRendererd.dll
                /DELAYLOAD:GrycePlatformd.dll
                /DELAYLOAD:GrycePhysicsd.dll>
            $<$<NOT:$<CONFIG:Debug>>:
                /DELAYLOAD:GryceCore.dll
                /DELAYLOAD:GryceRenderer.dll
                /DELAYLOAD:GrycePlatform.dll
                /DELAYLOAD:GrycePhysics.dll>
        )
        target_link_libraries(GryceGame PRIVATE delayimp)
    endif()
endif()
