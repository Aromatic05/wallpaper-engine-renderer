find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)

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

include_directories(. common host backend/scene/internal render output)

add_subdirectory(common)
add_subdirectory(host)
add_subdirectory(backend/scene/internal)
add_subdirectory(render)

add_library(
  ${PROJECT_NAME} STATIC
  common/Type.cpp
  backend/BuiltinBackendFactory.cpp
  backend/scene/compatibility/WESceneSource.cpp
  backend/scene/compatibility/WESceneSession.cpp
  backend/web/WebBackend.cpp
  runtime/WallpaperRuntime.cpp
  runtime/session/WallpaperSession.cpp
  backend/scene/compatibility/WESceneOutputTarget.cpp
  backend/scene/WESceneBackend.cpp
  backend/scene/internal/WPJson.cpp
  backend/scene/internal/WPPkgFs.cpp
  backend/scene/internal/wpscene/WPImageObject.cpp
  backend/scene/internal/wpscene/WPParticleObject.cpp
  backend/scene/internal/wpscene/WPMaterial.cpp
  backend/scene/internal/wpscene/WPScene.cpp
  backend/scene/internal/wpscene/WPLightObject.cpp
  backend/scene/internal/WPParticleParser.cpp
  backend/scene/internal/WPShaderParser.cpp
  backend/scene/internal/WPSceneParser.cpp
  backend/scene/internal/WPShaderValueUpdater.cpp
  backend/scene/internal/WPTexImageParser.cpp
  backend/scene/internal/WPSoundParser.cpp
  backend/scene/internal/WPMdlParser.cpp
  backend/scene/internal/WPPuppet.cpp
  backend/scene/legacy/SceneWallpaper.cpp)

set(InteralLib
    wpAudio
    wpLooper
    wpVulkanRender
    wpFs
    wpScene
    wpVulkan
    wpRGraph
    wpParticle
    wpTimer)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC wpUtils
  PRIVATE ${InteralLib} nlohmann_json ${LZ4_LIBRARIES}
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

target_include_directories(${PROJECT_NAME} PUBLIC . common host backend/scene/internal render output)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
