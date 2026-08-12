# Helper invoked by cmake/shaders.cmake to compile one GLSL shader to SPIR-V.
#
# The .spv outputs live in the source tree (res:/shaders/spirv) and multiple
# build trees (e.g. VS solution + Ninja cache + editor integration) can invoke
# the same custom command concurrently; a transient file lock on the shared
# output makes glslangValidator fail with exit 1. Retrying a few times rides
# over those locks instead of failing the whole build.
#
# Usage:
#   cmake -DGLSLANG=<glslangValidator> -DSOURCE=<in.glsl> \
#         -DOUTPUT=<out.spv> -P cmake/run_glslang.cmake

if(NOT DEFINED GLSLANG OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "run_glslang.cmake: GLSLANG/SOURCE/OUTPUT required")
endif()

execute_process(
    COMMAND "${GLSLANG}" -V "${SOURCE}" -o "${OUTPUT}"
    RESULT_VARIABLE rc
)

if(NOT rc EQUAL 0)
    foreach(attempt RANGE 1 5)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
        execute_process(
            COMMAND "${GLSLANG}" -V "${SOURCE}" -o "${OUTPUT}"
            RESULT_VARIABLE rc
        )
        if(rc EQUAL 0)
            break()
        endif()
    endforeach()
endif()

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "glslangValidator failed (exit ${rc}) compiling '${SOURCE}' -> '${OUTPUT}'. "
        "Is another build writing the same .spv concurrently?"
    )
endif()
