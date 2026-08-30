# Deploy a native library into the legacy WPF editor output directory.
#
# Expected -D variables:
#   GRYCE_EDITOR_BIN  absolute target directory
#   GRYCE_SRC         absolute path to the built library
#   GRYCE_NAME        output file name (normally $<TARGET_FILE_NAME:...>)

if(NOT DEFINED GRYCE_EDITOR_BIN OR NOT DEFINED GRYCE_SRC OR NOT DEFINED GRYCE_NAME)
    message(FATAL_ERROR "editor_deploy.cmake: GRYCE_EDITOR_BIN, GRYCE_SRC, and GRYCE_NAME are required")
endif()

if(NOT EXISTS "${GRYCE_SRC}")
    message(FATAL_ERROR "editor_deploy.cmake: source library not found: ${GRYCE_SRC}")
endif()

file(MAKE_DIRECTORY "${GRYCE_EDITOR_BIN}")
file(COPY_FILE "${GRYCE_SRC}" "${GRYCE_EDITOR_BIN}/${GRYCE_NAME}" ONLY_IF_DIFFERENT)
