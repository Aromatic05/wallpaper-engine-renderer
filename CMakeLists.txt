cmake_minimum_required(VERSION 3.14)
project(wallpaper-engine-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
include(ExternalProject)

include(CTest)

include_directories(SYSTEM third_party)

link_libraries(Eigen3::Eigen)

option(WP_ENABLE_SCENESCRIPT_RUNTIME "Enable SceneScript runtime build dependencies" ON)
option(WP_ENABLE_TEXT_RENDERING "Enable text rendering build dependencies" ON)

set(HANABI_DXC_ROOT "" CACHE PATH "Path to Hanabi staged DirectXShaderCompiler SDK")
set(HANABI_DXC_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectXShaderCompiler"
    CACHE PATH "Path to Hanabi vendored DirectXShaderCompiler checkout")
set(HANABI_BUILD_VENDORED_DXC ON CACHE BOOL "Build the vendored DXC checkout for WE shaders")

if(HANABI_BUILD_VENDORED_DXC AND EXISTS "${HANABI_DXC_SOURCE_DIR}/CMakeLists.txt")
    if(NOT HANABI_DXC_ROOT)
        set(HANABI_DXC_ROOT "${CMAKE_CURRENT_BINARY_DIR}/dxc-stage" CACHE PATH "Path to Hanabi staged DirectXShaderCompiler SDK" FORCE)
    endif()
    set(HANABI_DXC_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/dxc-build" CACHE PATH "Hanabi vendored DirectXShaderCompiler build directory")
    set(HANABI_DXC_INCLUDE_DIR "${HANABI_DXC_ROOT}/include")
    set(HANABI_DXCOMPILER_LIBRARY "${HANABI_DXC_ROOT}/lib/libdxcompiler.so")
    set(HANABI_DXC_EXECUTABLE "${HANABI_DXC_ROOT}/bin/dxc")

    file(MAKE_DIRECTORY "${HANABI_DXC_ROOT}/include" "${HANABI_DXC_ROOT}/lib" "${HANABI_DXC_ROOT}/bin")

    ExternalProject_Add(HanabiVendoredDxc
        SOURCE_DIR "${HANABI_DXC_SOURCE_DIR}"
        BINARY_DIR "${HANABI_DXC_BUILD_DIR}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
            -DCMAKE_INSTALL_PREFIX=${HANABI_DXC_ROOT}
            -DENABLE_SPIRV_CODEGEN=ON
            -DLLVM_TARGETS_TO_BUILD=None
            -DLLVM_ENABLE_EH=ON
            -DLLVM_ENABLE_RTTI=ON
            -DLLVM_INCLUDE_TESTS=OFF
            -DCLANG_INCLUDE_TESTS=OFF
            -DHLSL_INCLUDE_TESTS=OFF
            -DSPIRV_BUILD_TESTS=OFF
            -DLLVM_INCLUDE_DOCS=OFF
            -DCLANG_INCLUDE_DOCS=OFF
            -DLLVM_INCLUDE_EXAMPLES=OFF
            -DLLVM_BUILD_EXAMPLES=OFF
            -DCLANG_BUILD_EXAMPLES=OFF
            -DHLSL_BUILD_DXILCONV=OFF
            -DCLANG_ENABLE_ARCMT=OFF
            -DCLANG_ENABLE_STATIC_ANALYZER=OFF
        BUILD_COMMAND
            ${CMAKE_COMMAND} --build <BINARY_DIR> --target dxcompiler dxc
        INSTALL_COMMAND
            ${CMAKE_COMMAND} --build <BINARY_DIR> --target install-dxcompiler install-dxc install-dxc-headers
        BUILD_BYPRODUCTS
            "${HANABI_DXCOMPILER_LIBRARY}"
            "${HANABI_DXC_EXECUTABLE}"
            "${HANABI_DXC_ROOT}/include/dxc/dxcapi.h"
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE)

    add_library(Hanabi::DxCompiler SHARED IMPORTED GLOBAL)
    set_target_properties(Hanabi::DxCompiler PROPERTIES
        IMPORTED_LOCATION "${HANABI_DXCOMPILER_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HANABI_DXC_INCLUDE_DIR}")
    set(HANABI_HAS_DXC ON CACHE BOOL "Hanabi staged DXC SDK is available" FORCE)
    message(STATUS "Hanabi vendored DXC SDK will be staged at: ${HANABI_DXC_ROOT}")
elseif(HANABI_DXC_ROOT)
    find_path(HANABI_DXC_INCLUDE_DIR
        NAMES dxc/dxcapi.h
        PATHS "${HANABI_DXC_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(HANABI_DXCOMPILER_LIBRARY
        NAMES dxcompiler
        PATHS "${HANABI_DXC_ROOT}/lib" "${HANABI_DXC_ROOT}/lib64"
        NO_DEFAULT_PATH)
    find_program(HANABI_DXC_EXECUTABLE
        NAMES dxc
        PATHS "${HANABI_DXC_ROOT}/bin"
        NO_DEFAULT_PATH)

    if(HANABI_DXC_INCLUDE_DIR AND HANABI_DXCOMPILER_LIBRARY)
        add_library(Hanabi::DxCompiler SHARED IMPORTED GLOBAL)
        set_target_properties(Hanabi::DxCompiler PROPERTIES
            IMPORTED_LOCATION "${HANABI_DXCOMPILER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${HANABI_DXC_INCLUDE_DIR}")
        set(HANABI_HAS_DXC ON CACHE BOOL "Hanabi staged DXC SDK is available" FORCE)
        message(STATUS "Hanabi DXC SDK: ${HANABI_DXC_ROOT}")
    else()
        set(HANABI_HAS_DXC OFF CACHE BOOL "Hanabi staged DXC SDK is available" FORCE)
        message(WARNING "HANABI_DXC_ROOT was set to '${HANABI_DXC_ROOT}', but dxcapi.h or libdxcompiler was not found")
    endif()
else()
    find_path(HANABI_DXC_INCLUDE_DIR NAMES dxc/dxcapi.h)
    find_library(HANABI_DXCOMPILER_LIBRARY NAMES dxcompiler)
    find_program(HANABI_DXC_EXECUTABLE NAMES dxc)

    if(HANABI_DXC_INCLUDE_DIR AND HANABI_DXCOMPILER_LIBRARY)
        add_library(Hanabi::DxCompiler SHARED IMPORTED GLOBAL)
        set_target_properties(Hanabi::DxCompiler PROPERTIES
            IMPORTED_LOCATION "${HANABI_DXCOMPILER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${HANABI_DXC_INCLUDE_DIR}")
        set(HANABI_HAS_DXC ON CACHE BOOL "Hanabi staged DXC SDK is available" FORCE)
        message(STATUS "Hanabi system DXC SDK: ${HANABI_DXCOMPILER_LIBRARY}")
    else()
        set(HANABI_HAS_DXC OFF CACHE BOOL "Hanabi staged DXC SDK is available" FORCE)
        message(WARNING "Hanabi DXC SDK was not found; set HANABI_DXC_ROOT or install DirectXShaderCompiler")
    endif()
endif()

set(WP_QUICKJS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs"
    CACHE PATH "Path to the QuickJS checkout used for SceneScript runtime support")

if(WP_ENABLE_SCENESCRIPT_RUNTIME)
    if(NOT EXISTS "${WP_QUICKJS_DIR}/quickjs.c")
        message(FATAL_ERROR
                "WP_ENABLE_SCENESCRIPT_RUNTIME=ON requires QuickJS sources at "
                "'${WP_QUICKJS_DIR}'.")
    endif()

    add_library(
        wpquickjs STATIC
        ${WP_QUICKJS_DIR}/dtoa.c
        ${WP_QUICKJS_DIR}/libregexp.c
        ${WP_QUICKJS_DIR}/libunicode.c
        ${WP_QUICKJS_DIR}/quickjs.c)
    target_compile_definitions(wpquickjs PRIVATE QUICKJS_NG_BUILD _GNU_SOURCE)
    target_include_directories(wpquickjs PUBLIC ${WP_QUICKJS_DIR})

    find_library(M_LIBRARIES m)
    if(M_LIBRARIES)
        target_link_libraries(wpquickjs PUBLIC ${CMAKE_DL_LIBS} Threads::Threads m)
    else()
        target_link_libraries(wpquickjs PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
    endif()
    set_property(TARGET wpquickjs PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

add_subdirectory(third_party)
add_subdirectory(src)

if(BUILD_QML)
    find_package(Qt6 COMPONENTS Gui Quick Qml REQUIRED)

    set(CMAKE_AUTOMOC ON) 
    set(CMAKE_AUTORCC ON) 
    set(CMAKE_AUTOUIC ON)

    add_library(${PROJECT_NAME}-qml
    STATIC
        qml_helper/SceneBackend.cpp
        qml_helper/glExtra.cpp
    )
    target_link_libraries(${PROJECT_NAME}-qml
    PUBLIC
        Qt::Quick
        Qt::Gui
        glad
        wallpaper-engine-renderer-internal
    )
    target_include_directories(${PROJECT_NAME}-qml PUBLIC qml_helper third_party)

    set(CMAKE_AUTOMOC OFF) 
    set(CMAKE_AUTORCC OFF) 
    set(CMAKE_AUTOUIC OFF) 
endif()

if(BUILD_TESTING)
    add_subdirectory(src/test)
endif()

add_subdirectory(standalone_view)
