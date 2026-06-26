find_package(OpenGL REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
pkg_check_modules(PANGOFT2 REQUIRED pangoft2)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(FREETYPE2 REQUIRED freetype2)

option(ENABLE_RENDERDOC "Build with renderdoc api" OFF)

if(ENABLE_RENDERDOC)
  add_compile_definitions(ENABLE_RENDERDOC_API=1)
endif()

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

add_subdirectory(common)
add_subdirectory(host)
add_subdirectory(backend/scene)
add_subdirectory(backend/web)
add_subdirectory(render)

set(wallpaper_renderer_sources
  abi/WeRenderer.cpp
  common/Type.cpp
  backend/BuiltinBackendFactory.cpp
  backend/BuiltinSessionFactory.cpp
  api/scene/WEScene.cpp
  api/scene/WESceneOutput.cpp
  api/scene/WESceneSource.cpp
  api/web/Web.cpp
  host/HostServices.cpp
  output/OutputController.cpp
  runtime/WallpaperRuntime.cpp
  runtime/session/WallpaperSession.cpp)

set(InteralLib
    wpSceneBackendModule
)
if(TARGET wpWebBackend)
    list(APPEND InteralLib wpWebBackend)
endif()

add_library(${PROJECT_NAME}_obj OBJECT ${wallpaper_renderer_sources})
target_compile_definitions(
  ${PROJECT_NAME}_obj
  PRIVATE
    BUILD_WEWEB=$<BOOL:${BUILD_WEWEB}>)
target_include_directories(
  ${PROJECT_NAME}_obj
  PUBLIC ../include common/utils/include
  PRIVATE . common)
target_link_libraries(
  ${PROJECT_NAME}_obj
  PUBLIC wpUtils
  PRIVATE ${InteralLib})
target_compile_options(
  ${PROJECT_NAME}_obj
  PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden)
set_property(TARGET ${PROJECT_NAME}_obj PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(${PROJECT_NAME} SHARED $<TARGET_OBJECTS:${PROJECT_NAME}_obj>)
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    BUILD_WEWEB=$<BOOL:${BUILD_WEWEB}>)
target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC wpUtils
  PRIVATE ${InteralLib})

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC ../include common/utils/include
  PRIVATE . common)
target_link_options(
  ${PROJECT_NAME}
  PRIVATE
    -Wl,--exclude-libs=ALL)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
if(BUILD_WEWEB AND DEFINED Vulkan_LIBRARY AND DEFINED CEF_LIBRARY)
  get_filename_component(_wp_vulkan_libdir "${Vulkan_LIBRARY}" DIRECTORY)
  get_filename_component(_wp_cef_libdir "${CEF_LIBRARY}" DIRECTORY)
  set_property(TARGET ${PROJECT_NAME} PROPERTY BUILD_RPATH "${_wp_vulkan_libdir};${_wp_cef_libdir}")
  set_property(TARGET ${PROJECT_NAME} PROPERTY INSTALL_RPATH "${_wp_vulkan_libdir};${_wp_cef_libdir}")
  set_property(TARGET ${PROJECT_NAME} PROPERTY BUILD_RPATH_USE_ORIGIN FALSE)
endif()

add_library(${PROJECT_NAME}-internal STATIC $<TARGET_OBJECTS:${PROJECT_NAME}_obj>)
target_compile_definitions(
  ${PROJECT_NAME}-internal
  PRIVATE
    BUILD_WEWEB=$<BOOL:${BUILD_WEWEB}>)
target_link_libraries(
  ${PROJECT_NAME}-internal
  PUBLIC wpUtils
  PUBLIC ${InteralLib})
target_include_directories(
  ${PROJECT_NAME}-internal
  PUBLIC
    .
    common
    ../include
    common/utils/include)
set_property(TARGET ${PROJECT_NAME}-internal PROPERTY POSITION_INDEPENDENT_CODE ON)
