cmake_minimum_required(VERSION 3.21)
project(QuickGadgets VERSION 1.1.0 LANGUAGES CXX)
include(CTest)
if(BUILD_TESTING)
    add_executable(KeyboardBindingsTests tests/KeyboardBindingsTests.cpp)
    target_compile_features(KeyboardBindingsTests PRIVATE cxx_std_20)
    target_include_directories(KeyboardBindingsTests PRIVATE src)
    add_test(NAME KeyboardBindings COMMAND KeyboardBindingsTests)
endif()

add_library(QuickGadgets SHARED src/QuickGadgets.cpp)
target_compile_features(QuickGadgets PRIVATE cxx_std_20)
target_compile_definitions(QuickGadgets PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_library(QuickGadgetsBootstrap SHARED src/QuickGadgetsBootstrap.cpp)
target_compile_features(QuickGadgetsBootstrap PRIVATE cxx_std_20)
target_compile_definitions(QuickGadgetsBootstrap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(QuickGadgets PRIVATE /W4 /permissive-)
    target_compile_options(QuickGadgetsBootstrap PRIVATE /W4 /permissive-)
endif()
set_target_properties(QuickGadgets PROPERTIES
    OUTPUT_NAME "QuickGadgets"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
set_target_properties(QuickGadgetsBootstrap PROPERTIES
    OUTPUT_NAME "QuickGadgetsBootstrap"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_target(package
    COMMAND ${CMAKE_COMMAND}
        -DDLL_PATH=$<TARGET_FILE:QuickGadgets>
        -DBOOTSTRAP_DLL_PATH=$<TARGET_FILE:QuickGadgetsBootstrap>
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/package
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DSCRIPT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/PackageScript.cmake
    DEPENDS QuickGadgets QuickGadgetsBootstrap
    COMMENT "Creating versioned QuickGadgets package"
)
