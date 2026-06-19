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

add_library(
  ${PROJECT_NAME} STATIC
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
    wpWebBackend
)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC wpUtils
  PRIVATE ${InteralLib})

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC ../include common/utils/include
  PRIVATE . common)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
