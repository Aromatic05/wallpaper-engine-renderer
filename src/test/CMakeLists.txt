add_executable(
  wallpaper-session-contract-test
  runtime/wallpaper_session_contract_test.cpp)

target_link_libraries(wallpaper-session-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(wallpaper-session-contract-test PRIVATE ..)
target_compile_options(wallpaper-session-contract-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-session-contract-test COMMAND wallpaper-session-contract-test)

add_executable(
  wallpaper-public-api-test
  runtime/wallpaper_public_api_test.cpp)

target_link_libraries(wallpaper-public-api-test PRIVATE wallpaper-engine-renderer-internal)
target_compile_options(wallpaper-public-api-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-public-api-test COMMAND wallpaper-public-api-test)

add_executable(
  we-renderer-options-test
  abi/we_renderer_options_test.cpp)

target_link_libraries(we-renderer-options-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-renderer-options-test PRIVATE ..)
target_compile_options(we-renderer-options-test PRIVATE ${warn_opts})

add_test(NAME we-renderer-options-test COMMAND we-renderer-options-test)

add_executable(
  we-project-source-test
  abi/we_project_source_test.cpp)

target_link_libraries(we-project-source-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-project-source-test PRIVATE ..)
target_compile_options(we-project-source-test PRIVATE ${warn_opts})

add_test(NAME we-project-source-test COMMAND we-project-source-test)

add_executable(
  we-renderer-c-abi-test
  abi/we_renderer_c_abi_test.c)

target_link_libraries(we-renderer-c-abi-test PRIVATE ${PROJECT_NAME})
target_compile_options(we-renderer-c-abi-test PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME we-renderer-c-abi-test COMMAND we-renderer-c-abi-test)

add_test(
  NAME we-renderer-abi-symbols-test
  COMMAND ${CMAKE_COMMAND}
          -DLIBRARY=$<TARGET_FILE:${PROJECT_NAME}>
          -DNM=${CMAKE_NM}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_symbols_test.cmake)

add_executable(
  user-properties-json-test
  scene/user_properties_json_test.cpp)

target_link_libraries(user-properties-json-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(
  user-properties-json-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/settings
  ../backend/scene/internal/scene/include)
target_compile_options(user-properties-json-test PRIVATE ${warn_opts})

add_test(NAME user-properties-json-test COMMAND user-properties-json-test)

add_executable(
  wp-pkg-index-test
  scene/pkg/pkg_index_test.cpp)

target_link_libraries(wp-pkg-index-test PRIVATE wpSceneBackend)
target_include_directories(
  wp-pkg-index-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../common
  ../common/fs/include)
target_compile_options(wp-pkg-index-test PRIVATE ${warn_opts})

add_test(NAME wp-pkg-index-test COMMAND wp-pkg-index-test)

add_executable(
  mdl-format-test
  scene/mdl/format_test.cpp)

target_link_libraries(mdl-format-test PRIVATE wpSceneBackend)
target_include_directories(
  mdl-format-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../common
  ../common/fs/include)
target_compile_options(mdl-format-test PRIVATE ${warn_opts})

add_test(NAME mdl-format-test COMMAND mdl-format-test)

add_executable(
  tex-header-parser-test
  scene/tex/tex_header_parser_test.cpp)

target_link_libraries(tex-header-parser-test PRIVATE wpSceneBackend)
target_include_directories(
  tex-header-parser-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../common
  ../common/scene
  ../common/fs/include)
target_compile_options(tex-header-parser-test PRIVATE ${warn_opts})

add_test(NAME tex-header-parser-test COMMAND tex-header-parser-test)

add_executable(
  shader-audio-compat-test
  scene/shader/shader_audio_compat_test.cpp)

target_link_libraries(shader-audio-compat-test PRIVATE wpSceneBackend)
target_include_directories(
  shader-audio-compat-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/animation
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(shader-audio-compat-test PRIVATE ${warn_opts})

add_test(NAME shader-audio-compat-test COMMAND shader-audio-compat-test)

add_executable(
  shader-uniform-compat-test
  scene/shader/shader_uniform_compat_test.cpp)

target_link_libraries(shader-uniform-compat-test PRIVATE wpSceneBackend)
target_include_directories(
  shader-uniform-compat-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/animation
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(shader-uniform-compat-test PRIVATE ${warn_opts})

add_test(NAME shader-uniform-compat-test COMMAND shader-uniform-compat-test)

add_executable(
  corpus-manifest-test
  corpus/corpus_manifest_test.cpp)

target_link_libraries(corpus-manifest-test PRIVATE wpSceneBackend)
target_include_directories(
  corpus-manifest-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../common
  ../common/fs/include
  ../../third_party/nlohmann/include)
target_compile_definitions(
  corpus-manifest-test PRIVATE
  WE_CORPUS_MANIFEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/corpus/manifest.json")
target_compile_options(corpus-manifest-test PRIVATE ${warn_opts})

add_test(NAME corpus-manifest-test COMMAND corpus-manifest-test)

add_executable(
  we-renderer-scene-options-test
  abi/we_renderer_scene_options_test.cpp)

target_link_libraries(we-renderer-scene-options-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-renderer-scene-options-test PRIVATE ..)
target_compile_options(we-renderer-scene-options-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME we-renderer-scene-options-test COMMAND we-renderer-scene-options-test)

add_executable(
  output-controller-contract-test
  output/output_controller_contract_test.cpp)

target_link_libraries(output-controller-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(output-controller-contract-test PRIVATE ..)
target_compile_options(output-controller-contract-test PRIVATE ${warn_opts})

add_test(NAME output-controller-contract-test COMMAND output-controller-contract-test)

add_executable(
  shm-frame-swapchain-test
  output/shm_frame_swapchain_test.cpp)

target_link_libraries(shm-frame-swapchain-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(shm-frame-swapchain-test PRIVATE ..)
target_compile_options(shm-frame-swapchain-test PRIVATE ${warn_opts})

add_test(NAME shm-frame-swapchain-test COMMAND shm-frame-swapchain-test)

if(BUILD_WEVIDEO)
    add_executable(
      we-renderer-video-source-test
      abi/we_renderer_video_source_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(we-renderer-video-source-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(we-renderer-video-source-test PRIVATE ..)
    target_compile_options(we-renderer-video-source-test PRIVATE ${warn_opts})

    add_test(NAME we-renderer-video-source-test COMMAND we-renderer-video-source-test)

    add_executable(
      we-renderer-video-pipeline-test
      abi/we_renderer_video_pipeline_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(we-renderer-video-pipeline-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(we-renderer-video-pipeline-test PRIVATE ..)
    target_compile_options(we-renderer-video-pipeline-test PRIVATE ${warn_opts})

    add_test(NAME we-renderer-video-pipeline-test COMMAND we-renderer-video-pipeline-test)
endif()

if(BUILD_WEWEB)
    add_executable(
      web-backend-contract-test
      web/web_backend_contract_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(web-backend-contract-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(web-backend-contract-test PRIVATE .. web)
    target_compile_options(web-backend-contract-test PRIVATE ${warn_opts})

    add_test(NAME web-backend-contract-test COMMAND web-backend-contract-test)

    add_executable(
      we-renderer-web-source-test
      abi/we_renderer_web_source_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(we-renderer-web-source-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(we-renderer-web-source-test PRIVATE .. web)
    target_compile_options(we-renderer-web-source-test PRIVATE ${warn_opts})

    add_test(NAME we-renderer-web-source-test COMMAND we-renderer-web-source-test)

    add_executable(
      web-user-properties-test
      web/web_user_properties_test.cpp
      ../backend/web/internal/cef/PropertyBridge.cpp)

    target_include_directories(web-user-properties-test PRIVATE ..)
    target_compile_options(web-user-properties-test PRIVATE ${warn_opts})

    add_test(NAME web-user-properties-test COMMAND web-user-properties-test)

    add_executable(
      we-cef-helper-dlopen-test
      web/we_cef_helper_dlopen_test.cpp)

    target_link_libraries(we-cef-helper-dlopen-test PRIVATE ${CMAKE_DL_LIBS})
    target_include_directories(we-cef-helper-dlopen-test PRIVATE ..)
    target_compile_options(we-cef-helper-dlopen-test PRIVATE ${warn_opts})

    add_test(
      NAME we-cef-helper-dlopen-test
      COMMAND we-cef-helper-dlopen-test $<TARGET_FILE:we-cef-helper>)

    add_executable(
      we-renderer-input-event-test
      abi/we_renderer_input_event_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(we-renderer-input-event-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(we-renderer-input-event-test PRIVATE .. web)
    target_compile_options(we-renderer-input-event-test PRIVATE ${warn_opts})

    add_test(NAME we-renderer-input-event-test COMMAND we-renderer-input-event-test)
endif()

# Optional real-CEF smoke test. Off by default because the host
# needs a stageable CEF runtime; the test itself probes for
# libcef.so and Resources/locales and skips (exit 0) when they
# are not present. Opt in with -DWP_ENABLE_CEF_INTEGRATION_TEST=ON
# on a machine that has staged CEF.
option(WP_ENABLE_CEF_INTEGRATION_TEST "Build the real-CEF smoke test for the web backend" OFF)
if(WP_ENABLE_CEF_INTEGRATION_TEST AND BUILD_WEWEB)
    add_executable(web-cef-integration-test web/web_cef_integration_test.cpp)
    target_link_libraries(web-cef-integration-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(web-cef-integration-test PRIVATE ..)
    target_compile_options(web-cef-integration-test PRIVATE ${warn_opts})
    add_test(NAME web-cef-integration-test COMMAND web-cef-integration-test)
endif()

add_executable(
  host-services-contract-test
  host/host_services_contract_test.cpp)

target_link_libraries(host-services-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(host-services-contract-test PRIVATE ..)
target_compile_options(host-services-contract-test PRIVATE ${warn_opts})

add_test(NAME host-services-contract-test COMMAND host-services-contract-test)

add_executable(
  sound-spectrum-dsp-test
  scene/sound_spectrum_dsp_test.cpp)

target_include_directories(sound-spectrum-dsp-test PRIVATE ..)
target_compile_options(sound-spectrum-dsp-test PRIVATE ${warn_opts})

add_test(NAME sound-spectrum-dsp-test COMMAND sound-spectrum-dsp-test)

add_executable(
  resource-correctness-test
  scene/resource_correctness_test.cpp)

target_link_libraries(resource-correctness-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(
  resource-correctness-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/animation
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../render/rendergraph/include
  ../render/vulkan/include)
target_compile_options(resource-correctness-test PRIVATE ${warn_opts})

add_test(NAME resource-correctness-test COMMAND resource-correctness-test)

add_executable(
  effect-rendergraph-correctness-test
  scene/effect_rendergraph_correctness_test.cpp)

target_link_libraries(effect-rendergraph-correctness-test PRIVATE wallpaper-engine-renderer-internal wpScene wpSceneBackend)
target_include_directories(
  effect-rendergraph-correctness-test PRIVATE
  ..
  ../render/rendergraph/include
  ../backend/scene/internal/scene/include
  ../backend/scene/internal
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/animation
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/utils/include
  ../render/vulkan/include)
target_compile_options(effect-rendergraph-correctness-test PRIVATE ${warn_opts})

add_test(NAME effect-rendergraph-correctness-test COMMAND effect-rendergraph-correctness-test)

add_executable(
  particle-regression-test
  scene/particle_regression_test.cpp)

target_link_libraries(particle-regression-test PRIVATE wallpaper-engine-renderer-internal wpScene wpSceneBackend)
target_include_directories(
  particle-regression-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/particle/include
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/animation
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(particle-regression-test PRIVATE ${warn_opts})

add_test(NAME particle-regression-test COMMAND particle-regression-test)

add_executable(
  synthetic-image-parser-test
  scene/synthetic_image_parser_test.cpp)

target_link_libraries(synthetic-image-parser-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  synthetic-image-parser-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene)
target_compile_options(synthetic-image-parser-test PRIVATE ${warn_opts})

add_test(NAME synthetic-image-parser-test COMMAND synthetic-image-parser-test)

add_executable(
  scene-parity-foundation-test
  scene/scene_parity_foundation_test.cpp)

target_link_libraries(scene-parity-foundation-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  scene-parity-foundation-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(scene-parity-foundation-test PRIVATE ${warn_opts})

add_test(NAME scene-parity-foundation-test COMMAND scene-parity-foundation-test)

add_executable(
  node-transform-resolver-test
  scene/node_transform_resolver_test.cpp)

target_link_libraries(node-transform-resolver-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  node-transform-resolver-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/shader
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(node-transform-resolver-test PRIVATE ${warn_opts})

add_test(NAME node-transform-resolver-test COMMAND node-transform-resolver-test)

add_executable(
  text-scene-model-test
  scene/text/text_scene_model_test.cpp)

target_link_libraries(text-scene-model-test PRIVATE ${PROJECT_NAME} wpScene wpSceneBackend)
target_include_directories(
  text-scene-model-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../common
  ../common/scene
  ../common/fs/include
  ../../third_party/nlohmann/include)
target_compile_options(text-scene-model-test PRIVATE ${warn_opts})

add_test(NAME text-scene-model-test COMMAND text-scene-model-test)

add_executable(
  text-layer-runtime-test
  scene/text/text_layer_runtime_test.cpp)

target_link_libraries(text-layer-runtime-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  text-layer-runtime-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../../third_party/nlohmann/include)
target_compile_options(text-layer-runtime-test PRIVATE ${warn_opts})

add_test(NAME text-layer-runtime-test COMMAND text-layer-runtime-test)

add_executable(
  text-clear-pass-test
  scene/text/text_clear_pass_test.cpp)

target_link_libraries(text-clear-pass-test PRIVATE ${PROJECT_NAME} wpVulkanRender wpRGraph)
target_include_directories(
  text-clear-pass-test PRIVATE
  ..
  ../render
  ../render/rendergraph/include
  ../render/vulkan/include
  ../render/vulkanrender
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../../third_party/nlohmann/include)
target_compile_options(text-clear-pass-test PRIVATE ${warn_opts})

add_test(NAME text-clear-pass-test COMMAND text-clear-pass-test)

add_executable(
  video-texture-cache-test
  video/video_texture_cache_test.cpp)

target_link_libraries(video-texture-cache-test PRIVATE ${PROJECT_NAME} wpVulkan)
target_include_directories(
  video-texture-cache-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../render/vulkan/include)
target_compile_options(video-texture-cache-test PRIVATE ${warn_opts})

add_test(NAME video-texture-cache-test COMMAND video-texture-cache-test)

add_executable(
  migrated-features-regression-test
  scene/migrated_features_regression_test.cpp)

target_link_libraries(
  migrated-features-regression-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend
  wpVulkanRender
  wpRGraph)
target_include_directories(
  migrated-features-regression-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/utils/include
  ../render
  ../render/rendergraph/include
  ../render/vulkan/include
  ../render/vulkanrender
  ../../third_party/nlohmann/include)
target_compile_options(migrated-features-regression-test PRIVATE ${warn_opts})

add_test(NAME migrated-features-regression-test COMMAND migrated-features-regression-test)

add_executable(
  dynamic-value-user-setting-test
  scene/dynamic_value_user_setting_test.cpp)

target_link_libraries(dynamic-value-user-setting-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  dynamic-value-user-setting-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../common
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(dynamic-value-user-setting-test PRIVATE ${warn_opts})

add_test(NAME dynamic-value-user-setting-test COMMAND dynamic-value-user-setting-test)

add_executable(
  property-animation-test
  scene/property_animation_test.cpp)

target_link_libraries(property-animation-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  property-animation-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/settings
  ../common
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(property-animation-test PRIVATE ${warn_opts})

add_test(NAME property-animation-test COMMAND property-animation-test)

if(WP_ENABLE_SCENESCRIPT_RUNTIME)
  add_executable(
    scenescript-host-test
    scene/script/scenescript_host_test.cpp)

  target_link_libraries(scenescript-host-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
  target_include_directories(
    scenescript-host-test PRIVATE
    ..
    ../backend/scene/internal
    ../backend/scene/internal/animation
    ../backend/scene/internal/scene/include
    ../backend/scene/internal/scenescript
    ../backend/scene/internal/settings
    ../backend/scene/internal/text
    ../common
    ../common/scene
    ../common/utils/include
    ../../third_party/nlohmann/include)
  # This test intentionally uses assert() around registration/mount calls whose return values are
  # part of the contract. Keep those calls present even when the project build type defines NDEBUG.
  target_compile_options(scenescript-host-test PRIVATE ${warn_opts} -UNDEBUG)

  add_test(NAME scenescript-host-test COMMAND scenescript-host-test)

  add_executable(
    scenescript-viewport-vector-test
    scene/script/scenescript_viewport_vector_test.cpp)

  target_link_libraries(scenescript-viewport-vector-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
  target_include_directories(
    scenescript-viewport-vector-test PRIVATE
    ..
    ../backend/scene/internal
    ../backend/scene/internal/animation
    ../backend/scene/internal/scene/include
    ../backend/scene/internal/scenescript
    ../backend/scene/internal/settings
    ../backend/scene/internal/text
    ../common
    ../common/scene
    ../common/utils/include
    ../../third_party/nlohmann/include)
  target_compile_options(scenescript-viewport-vector-test PRIVATE ${warn_opts})

  add_test(NAME scenescript-viewport-vector-test COMMAND scenescript-viewport-vector-test)

  add_executable(
    scenescript-runtime-test
    scene/script/scenescript_runtime_test.cpp)

  target_link_libraries(scenescript-runtime-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
  target_include_directories(
    scenescript-runtime-test PRIVATE
    ..
    ../backend/scene/internal
    ../backend/scene/internal/animation
    ../backend/scene/internal/scene/include
    ../backend/scene/internal/scenescript
    ../backend/scene/internal/settings
    ../backend/scene/internal/text
    ../common
    ../common/scene
    ../common/utils/include
    ../../third_party/nlohmann/include)
  target_compile_options(scenescript-runtime-test PRIVATE ${warn_opts})

  add_test(NAME scenescript-runtime-test COMMAND scenescript-runtime-test)

endif()

add_test(
  NAME wallpaper-architecture-guard-test
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/architecture/architecture_guard_test.cmake)