cmake_minimum_required(VERSION 3.14)
project(wallpaper-engine-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

include(CTest)

include_directories(SYSTEM third_party)

link_libraries(Eigen3::Eigen)

option(WP_ENABLE_SCENESCRIPT_RUNTIME "Enable SceneScript runtime build dependencies" OFF)
option(WP_ENABLE_TEXT_RENDERING "Enable text rendering build dependencies" OFF)

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
        ${PROJECT_NAME}
    )
    target_include_directories(${PROJECT_NAME}-qml PUBLIC qml_helper third_party)

    set(CMAKE_AUTOMOC OFF) 
    set(CMAKE_AUTORCC OFF) 
    set(CMAKE_AUTOUIC OFF) 
endif()

if(BUILD_TESTING)
    add_subdirectory(src/test)
endif()
