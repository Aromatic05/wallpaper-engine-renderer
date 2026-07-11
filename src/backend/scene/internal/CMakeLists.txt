set(LIB_NAME wpSceneBackend)

add_library(
  ${LIB_NAME} STATIC
  resources/CreateScenePackageFs.cpp
  settings/WPDynamicValue.cpp
  settings/WPUserPropertiesJson.cpp
  resources/WPJson.cpp
  resources/WPPkgFs.cpp
  resources/WPPkgIndex.cpp
  transform/WPNodeTransformResolver.cpp
  parser/WPParticleParser.cpp
  animation/WPPropertyAnimation.cpp
  scenescript/WPSceneScriptHost.cpp
  scenescript/WPSceneScriptMedia.cpp
  parser/WPShaderParser.cpp
  parser/WPSceneParser.cpp
  shader/WPShaderValueUpdater.cpp
  parser/WPSceneLayerMetadata.cpp
  parser/WPSyntheticImageParser.cpp
  parser/WPTexImageParser.cpp
  parser/WPTexHeaderParser.cpp
  text/WPTextLayer.cpp
  settings/WPUserSetting.cpp
  parser/WPSoundParser.cpp
  parser/WPMdlParser.cpp
  parser/mdl/Format.cpp
  parser/mdl/Mesh.cpp
  parser/mdl/PuppetSemantics.cpp
  parser/particle/Override.cpp
  animation/WPPuppet.cpp
  parser/mdl/Section.cpp
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
      scenescript/WPScriptRuntime.cpp)
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

target_link_libraries(${LIB_NAME} PRIVATE
  ${PANGOCAIRO_LIBRARIES}
  ${PANGOFT2_LIBRARIES}
  ${FONTCONFIG_LIBRARIES}
  ${FREETYPE2_LIBRARIES})
target_include_directories(${LIB_NAME} PRIVATE
  ${PANGOCAIRO_INCLUDE_DIRS}
  ${PANGOFT2_INCLUDE_DIRS}
  ${FONTCONFIG_INCLUDE_DIRS}
  ${FREETYPE2_INCLUDE_DIRS})
target_compile_options(${LIB_NAME} PRIVATE
  ${PANGOCAIRO_CFLAGS_OTHER}
  ${PANGOFT2_CFLAGS_OTHER}
  ${FONTCONFIG_CFLAGS_OTHER}
  ${FREETYPE2_CFLAGS_OTHER})

target_include_directories(${LIB_NAME} PRIVATE
  .
  ../../..
  ../../../common/scene
  animation
  parser
  resources
  scenescript
  settings
  shader
  text
  transform
  scene/include
  scene/include/scene
  particle/include
  particle/include/particle)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
target_compile_definitions(${LIB_NAME} PRIVATE WP_ENABLE_SCENESCRIPT_RUNTIME=$<BOOL:${WP_ENABLE_SCENESCRIPT_RUNTIME}>)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

if(WP_ENABLE_SCENESCRIPT_RUNTIME)
  target_link_libraries(${LIB_NAME} PRIVATE wpquickjs)
endif()

add_subdirectory(scene)
add_subdirectory(particle)
