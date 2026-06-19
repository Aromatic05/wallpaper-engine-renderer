set(LIB_NAME wpSceneBackend)

add_library(
  ${LIB_NAME} STATIC
  CreateScenePackageFs.cpp
  WPDynamicValue.cpp
  WPJson.cpp
  WPPkgFs.cpp
  WPParticleParser.cpp
  WPPropertyAnimation.cpp
  WPSceneScriptHost.cpp
  WPSceneScriptMedia.cpp
  WPShaderParser.cpp
  WPSceneParser.cpp
  WPShaderValueUpdater.cpp
  WPSyntheticImageParser.cpp
  WPTexImageParser.cpp
  WPTextLayer.cpp
  WPUserSetting.cpp
  WPSoundParser.cpp
  WPMdlParser.cpp
  WPPuppet.cpp
  wpscene/WPImageObject.cpp
  wpscene/WPEffect.cpp
  wpscene/WPParticleObject.cpp
  wpscene/WPMaterial.cpp
  wpscene/WPScene.cpp
  wpscene/WPLightObject.cpp
  wpscene/WPTextObject.cpp
  engine/WESceneBackend.cpp
  engine/WESceneRenderPlanBuilder.cpp
  engine/WESceneRuntimeDriver.cpp)

if(WP_ENABLE_SCENESCRIPT_RUNTIME)
  target_sources(
    ${LIB_NAME}
    PRIVATE
      WPScriptRuntime.cpp)
endif()

target_link_libraries(
  ${LIB_NAME}
  PUBLIC wpUtils
  PRIVATE wpAudio
          wpLooper
          wpVulkanRender
          wpFs
          wpScene
          wpVulkan
          wpRGraph
          wpParticle
          wpTimer
          nlohmann_json
          ${LZ4_LIBRARIES}
          ${CMAKE_THREAD_LIBS_INIT}
          ${CMAKE_DL_LIBS})

target_include_directories(${LIB_NAME} PRIVATE . ../../..)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
target_compile_definitions(${LIB_NAME} PRIVATE WP_ENABLE_SCENESCRIPT_RUNTIME=$<BOOL:${WP_ENABLE_SCENESCRIPT_RUNTIME}>)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

if(WP_ENABLE_SCENESCRIPT_RUNTIME)
  target_link_libraries(${LIB_NAME} PRIVATE wpquickjs)
endif()

add_subdirectory(scene)
add_subdirectory(particle)
