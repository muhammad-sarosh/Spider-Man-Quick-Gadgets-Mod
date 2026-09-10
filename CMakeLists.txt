cmake_minimum_required(VERSION 3.21)
project(QuickGadgets VERSION 0.1.0 LANGUAGES CXX)

add_library(QuickGadgets SHARED src/QuickGadgets.cpp)
target_compile_features(QuickGadgets PRIVATE cxx_std_20)
target_compile_definitions(QuickGadgets PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(QuickGadgets PRIVATE /W4 /permissive-)
endif()
set_target_properties(QuickGadgets PROPERTIES
    OUTPUT_NAME "QuickGadgets"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_target(package
    COMMAND ${CMAKE_COMMAND}
        -DDLL_PATH=$<TARGET_FILE:QuickGadgets>
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/package
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DSCRIPT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/PackageScript.cmake
    DEPENDS QuickGadgets
    COMMENT "Creating QuickGadgets.script"
)
