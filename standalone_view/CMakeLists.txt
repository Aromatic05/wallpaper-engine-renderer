cmake_minimum_required(VERSION 3.13)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project("sceneviewer")

set(DEFAULT_BUILD_TYPE "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
endif()

option(BUILD_QML "Build qml scene viewer" ${BUILD_QML})

find_package(PkgConfig REQUIRED)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

add_compile_options(
    $<$<CONFIG:Debug>:-ggdb>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-fsanitize=address>
)

add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address>
)

pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

set(WAYLAND_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
file(MAKE_DIRECTORY ${WAYLAND_GEN_DIR})

set(XDG_SHELL_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland/xdg-shell.xml)
set(LINUX_DMABUF_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland/linux-dmabuf-v1.xml)

set(XDG_SHELL_HEADER ${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h)
set(XDG_SHELL_CODE ${WAYLAND_GEN_DIR}/xdg-shell-protocol.c)
set(LINUX_DMABUF_HEADER ${WAYLAND_GEN_DIR}/linux-dmabuf-v1-client-protocol.h)
set(LINUX_DMABUF_CODE ${WAYLAND_GEN_DIR}/linux-dmabuf-v1-protocol.c)

add_custom_command(
    OUTPUT ${XDG_SHELL_HEADER}
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${XDG_SHELL_HEADER}
    DEPENDS ${XDG_SHELL_XML}
    VERBATIM
)

add_custom_command(
    OUTPUT ${XDG_SHELL_CODE}
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_CODE}
    DEPENDS ${XDG_SHELL_XML}
    VERBATIM
)

add_custom_command(
    OUTPUT ${LINUX_DMABUF_HEADER}
    COMMAND ${WAYLAND_SCANNER} client-header ${LINUX_DMABUF_XML} ${LINUX_DMABUF_HEADER}
    DEPENDS ${LINUX_DMABUF_XML}
    VERBATIM
)

add_custom_command(
    OUTPUT ${LINUX_DMABUF_CODE}
    COMMAND ${WAYLAND_SCANNER} private-code ${LINUX_DMABUF_XML} ${LINUX_DMABUF_CODE}
    DEPENDS ${LINUX_DMABUF_XML}
    VERBATIM
)

add_executable(${PROJECT_NAME}
    waylandviewer.cpp
    ${XDG_SHELL_HEADER}
    ${XDG_SHELL_CODE}
    ${LINUX_DMABUF_HEADER}
    ${LINUX_DMABUF_CODE}
)
target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        ${WAYLAND_GEN_DIR}
)
target_link_libraries(${PROJECT_NAME}
    PUBLIC
        ${WAYLAND_CLIENT_LIBRARIES}
        wallpaper-engine-renderer
)

if(BUILD_QML)
    find_package(QT NAMES Qt6 Qt5 COMPONENTS Gui Quick Qml REQUIRED)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Gui Quick Qml REQUIRED)

    set(CMAKE_AUTOMOC ON) 
    set(CMAKE_AUTORCC ON) 
    set(CMAKE_AUTOUIC ON)

    add_executable(${PROJECT_NAME}-qml
        qmlviewer.cpp
        main.qml
        pkg.qrc
    )
    target_link_libraries(${PROJECT_NAME}-qml
        PUBLIC 
        wallpaper-engine-renderer-qml
    )
    set(CMAKE_AUTOMOC OFF) 
    set(CMAKE_AUTORCC OFF) 
    set(CMAKE_AUTOUIC OFF)
endif()
