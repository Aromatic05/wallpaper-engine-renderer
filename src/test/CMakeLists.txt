add_executable(
  wallpaper-session-contract-test
  runtime/wallpaper_session_contract_test.cpp)

target_link_libraries(wallpaper-session-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(wallpaper-session-contract-test PRIVATE ..)
target_compile_options(wallpaper-session-contract-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-session-contract-test COMMAND wallpaper-session-contract-test)

add_executable(
  wallpaper-session-isolation-test
  runtime/wallpaper_session_isolation_test.cpp)

target_link_libraries(wallpaper-session-isolation-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(wallpaper-session-isolation-test PRIVATE ..)
target_compile_options(wallpaper-session-isolation-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME wallpaper-session-isolation-test COMMAND wallpaper-session-isolation-test)

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
  we-renderer-config-test
  abi/we_renderer_config_test.cpp)

target_link_libraries(we-renderer-config-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-renderer-config-test PRIVATE ..)
target_compile_options(we-renderer-config-test PRIVATE ${warn_opts})

add_test(NAME we-renderer-config-test COMMAND we-renderer-config-test)

add_executable(
  we-renderer-frame-status-test
  abi/we_renderer_frame_status_test.cpp)

target_link_libraries(we-renderer-frame-status-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-renderer-frame-status-test PRIVATE ..)
target_compile_options(we-renderer-frame-status-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME we-renderer-frame-status-test COMMAND we-renderer-frame-status-test)

add_executable(
  we-renderer-frame-ready-test
  abi/we_renderer_frame_ready_test.cpp)

target_link_libraries(we-renderer-frame-ready-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-renderer-frame-ready-test PRIVATE ..)
target_compile_options(we-renderer-frame-ready-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME we-renderer-frame-ready-test COMMAND we-renderer-frame-ready-test)

add_executable(
  we-renderer-runtime-test
  abi/we_renderer_runtime_test.cpp)

target_link_libraries(we-renderer-runtime-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(we-renderer-runtime-test PRIVATE ..)
target_compile_options(we-renderer-runtime-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME we-renderer-runtime-test COMMAND we-renderer-runtime-test)

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

find_program(WP_RUSTC_EXECUTABLE rustc)
if(WP_RUSTC_EXECUTABLE)
  add_test(
    NAME we-renderer-rust-abi-test
    COMMAND ${CMAKE_COMMAND}
            -DRUSTC=${WP_RUSTC_EXECUTABLE}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/we_renderer_rust_abi_test.rs
            -DLIBRARY=$<TARGET_FILE:${PROJECT_NAME}>
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/we-renderer-rust-abi-test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/abi/rust_abi_test.cmake)
else()
  message(WARNING "rustc not found; Rust ABI consumer test is disabled")
endif()

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
  mdl-mesh-test
  scene/mdl/mesh_test.cpp)

target_link_libraries(mdl-mesh-test PRIVATE wpSceneBackend)
target_include_directories(
  mdl-mesh-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../common
  ../common/fs/include)
target_compile_options(mdl-mesh-test PRIVATE ${warn_opts})

add_test(NAME mdl-mesh-test COMMAND mdl-mesh-test)

add_executable(
  mdl-section-test
  scene/mdl/section_test.cpp)

target_link_libraries(mdl-section-test PRIVATE wpSceneBackend)
target_include_directories(
  mdl-section-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../common
  ../common/fs/include)
target_compile_options(mdl-section-test PRIVATE ${warn_opts})

add_test(NAME mdl-section-test COMMAND mdl-section-test)

add_executable(
  mdl-parser-integration-test
  scene/mdl/parser_integration_test.cpp)

target_link_libraries(mdl-parser-integration-test PRIVATE wpSceneBackend)
target_include_directories(
  mdl-parser-integration-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../common
  ../common/scene
  ../common/fs/include)
target_compile_options(mdl-parser-integration-test PRIVATE ${warn_opts})

add_test(NAME mdl-parser-integration-test COMMAND mdl-parser-integration-test)

add_executable(
  mdl-puppet-semantics-test
  scene/mdl/puppet_semantics_test.cpp)

target_link_libraries(mdl-puppet-semantics-test PRIVATE wpSceneBackend)
target_include_directories(
  mdl-puppet-semantics-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../common
  ../common/scene
  ../common/fs/include)
target_compile_options(mdl-puppet-semantics-test PRIVATE ${warn_opts})

add_test(NAME mdl-puppet-semantics-test COMMAND mdl-puppet-semantics-test)

add_executable(
  scene-object-schema-test
  scene/schema/object_schema_test.cpp)

target_link_libraries(scene-object-schema-test PRIVATE wpSceneBackend)
target_include_directories(
  scene-object-schema-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(scene-object-schema-test PRIVATE ${warn_opts})

add_test(NAME scene-object-schema-test COMMAND scene-object-schema-test)

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

add_library(
  corpus-parser-probe-support STATIC
  corpus/parser_probe.cpp)

target_link_libraries(corpus-parser-probe-support PUBLIC wallpaper-engine-renderer-internal)
target_include_directories(
  corpus-parser-probe-support PUBLIC
  .
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../common
  ../common/scene
  ../common/fs/include
  ../../third_party/nlohmann/include)
target_compile_options(corpus-parser-probe-support PRIVATE ${warn_opts})

add_executable(
  corpus-parser-probe
  corpus/parser_probe_main.cpp)
target_link_libraries(corpus-parser-probe PRIVATE corpus-parser-probe-support)
target_compile_options(corpus-parser-probe PRIVATE ${warn_opts})

add_executable(
  corpus-parser-probe-test
  corpus/parser_probe_test.cpp)
target_link_libraries(corpus-parser-probe-test PRIVATE corpus-parser-probe-support)
target_compile_definitions(
  corpus-parser-probe-test PRIVATE
  WE_PARSER_PROBE_SNAPSHOT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/corpus/synthetic/parser_probe_snapshot.json")
target_compile_options(corpus-parser-probe-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME corpus-parser-probe-test COMMAND corpus-parser-probe-test)

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
  texture-output-contract-test
  output/texture_output_contract_test.cpp)

target_link_libraries(texture-output-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(texture-output-contract-test PRIVATE ..)
target_compile_options(texture-output-contract-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME texture-output-contract-test COMMAND texture-output-contract-test)

add_executable(
  output-resize-integration-test
  output/output_resize_integration_test.cpp)

target_link_libraries(output-resize-integration-test PRIVATE wallpaper-engine-renderer-internal "$<LINK_GROUP:RESCAN,wpVulkanRender,wpSceneBackend>")
target_include_directories(
  output-resize-integration-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/animation
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../render/rendergraph/include
  ../render/vulkan/include
  ../render/vulkanrender
  ../render/vulkanrender/include)
target_compile_options(output-resize-integration-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME output-resize-integration-test COMMAND output-resize-integration-test)

add_executable(
  final-output-msaa-test
  output/final_output_msaa_test.cpp)

target_link_libraries(final-output-msaa-test PRIVATE wpVulkanRender)
target_compile_options(final-output-msaa-test PRIVATE ${warn_opts})

add_test(NAME final-output-msaa-test COMMAND final-output-msaa-test)

add_executable(
  final-output-msaa-integration-test
  output/final_output_msaa_integration_test.cpp)

target_link_libraries(final-output-msaa-integration-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(
  final-output-msaa-integration-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/animation
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../render/rendergraph/include
  ../render/vulkan/include
  ../render/vulkanrender
  ../render/vulkanrender/include)
target_compile_options(final-output-msaa-integration-test PRIVATE ${warn_opts})

add_test(NAME final-output-msaa-integration-test COMMAND final-output-msaa-integration-test)

add_executable(
  sceneviewer-arg-test
  output/sceneviewer_arg_test.cpp)

target_compile_options(sceneviewer-arg-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME sceneviewer-arg-test COMMAND sceneviewer-arg-test)

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
    target_compile_options(we-renderer-video-source-test PRIVATE ${warn_opts} -UNDEBUG)

    add_test(NAME we-renderer-video-source-test COMMAND we-renderer-video-source-test)

    add_executable(
      we-renderer-video-pipeline-test
      abi/we_renderer_video_pipeline_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(we-renderer-video-pipeline-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(we-renderer-video-pipeline-test PRIVATE ..)
    target_compile_options(we-renderer-video-pipeline-test PRIVATE ${warn_opts} -UNDEBUG)

    add_test(NAME we-renderer-video-pipeline-test COMMAND we-renderer-video-pipeline-test)
endif()

if(BUILD_WEWEB)
    add_executable(
      web-backend-contract-test
      web/web_backend_contract_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(web-backend-contract-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(web-backend-contract-test PRIVATE .. web)
    target_compile_options(web-backend-contract-test PRIVATE ${warn_opts} -UNDEBUG)

    add_test(NAME web-backend-contract-test COMMAND web-backend-contract-test)

    add_executable(
      we-renderer-web-source-test
      abi/we_renderer_web_source_test.cpp
      ../abi/WeRenderer.cpp)

    target_link_libraries(we-renderer-web-source-test PRIVATE wallpaper-engine-renderer-internal)
    target_include_directories(we-renderer-web-source-test PRIVATE .. web)
    target_compile_options(we-renderer-web-source-test PRIVATE ${warn_opts} -UNDEBUG)

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
    add_dependencies(web-cef-integration-test we-cef-helper)
    add_test(NAME web-cef-integration-test COMMAND web-cef-integration-test)
    set_tests_properties(
      web-cef-integration-test
      PROPERTIES ENVIRONMENT "WE_CEF_HELPER_PATH=$<TARGET_FILE:we-cef-helper>")
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
target_compile_options(effect-rendergraph-correctness-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME effect-rendergraph-correctness-test COMMAND effect-rendergraph-correctness-test)

add_executable(
  linked-solid-composite-test
  scene/composition/linked_solid_composite_test.cpp)

target_link_libraries(
  linked-solid-composite-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend)
target_include_directories(
  linked-solid-composite-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../render/rendergraph/include
  ../render/vulkan/include
  ../../third_party/nlohmann/include)
target_compile_options(linked-solid-composite-test PRIVATE ${warn_opts})

add_test(NAME linked-solid-composite-test COMMAND linked-solid-composite-test)

add_executable(
  image-color-blend-test
  scene/composition/image_color_blend_test.cpp)

target_link_libraries(
  image-color-blend-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend)
target_include_directories(
  image-color-blend-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../render/rendergraph/include
  ../render/vulkan/include
  ../../third_party/nlohmann/include)
target_compile_options(image-color-blend-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME image-color-blend-test COMMAND image-color-blend-test)

add_executable(
  image-alpha-visibility-test
  scene/composition/image_alpha_visibility_test.cpp)

target_link_libraries(
  image-alpha-visibility-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend)
target_include_directories(
  image-alpha-visibility-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../render/rendergraph/include
  ../render/vulkan/include
  ../../third_party/nlohmann/include)
target_compile_options(image-alpha-visibility-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME image-alpha-visibility-test COMMAND image-alpha-visibility-test)

add_executable(
  effect-compatibility-test
  scene/composition/effect_compatibility_test.cpp)

target_link_libraries(
  effect-compatibility-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend
  wpTimer)
target_include_directories(
  effect-compatibility-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../host/timer/include
  ../render/rendergraph/include
  ../render/vulkan/include
  ../../third_party/nlohmann/include)
target_compile_options(effect-compatibility-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME effect-compatibility-test COMMAND effect-compatibility-test)

add_executable(
  media-thumbnail-fallback-test
  scene/media/media_thumbnail_fallback_test.cpp)

target_link_libraries(
  media-thumbnail-fallback-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend)
target_include_directories(
  media-thumbnail-fallback-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../render/rendergraph/include
  ../render/vulkan/include
  ../../third_party/nlohmann/include)
target_compile_options(media-thumbnail-fallback-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME media-thumbnail-fallback-test COMMAND media-thumbnail-fallback-test)

add_executable(
  text-effect-composition-test
  scene/text/text_effect_composition_test.cpp)

target_link_libraries(
  text-effect-composition-test PRIVATE
  ${PROJECT_NAME}
  wpSceneBackend)
target_include_directories(
  text-effect-composition-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../render/rendergraph/include
  ../render/vulkan/include
  ../../third_party/nlohmann/include)
target_compile_options(text-effect-composition-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME text-effect-composition-test COMMAND text-effect-composition-test)

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
target_compile_options(particle-regression-test PRIVATE ${warn_opts} -UNDEBUG)

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
  scene-camera-transform-test
  scene/camera_transform_test.cpp)

target_link_libraries(scene-camera-transform-test PRIVATE ${PROJECT_NAME} wpScene)
target_include_directories(
  scene-camera-transform-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../common
  ../common/scene
  ../common/utils/include)
target_compile_options(scene-camera-transform-test PRIVATE ${warn_opts})

add_test(NAME scene-camera-transform-test COMMAND scene-camera-transform-test)

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
  parallax-propagation-test
  scene/parallax_propagation_test.cpp)

target_link_libraries(parallax-propagation-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  parallax-propagation-test PRIVATE
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
target_compile_options(parallax-propagation-test PRIVATE ${warn_opts})

add_test(NAME parallax-propagation-test COMMAND parallax-propagation-test)

add_executable(
  sound-playback-policy-test
  scene/sound_playback_policy_test.cpp)

target_link_libraries(sound-playback-policy-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  sound-playback-policy-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../host/audio/include
  ../../third_party/nlohmann/include)
target_compile_options(sound-playback-policy-test PRIVATE ${warn_opts} -UNDEBUG)

add_test(NAME sound-playback-policy-test COMMAND sound-playback-policy-test)

add_executable(
  image-alignment-test
  scene/image_alignment_test.cpp)

target_link_libraries(image-alignment-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  image-alignment-test PRIVATE
  ..
  ../../include
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/shader
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(image-alignment-test PRIVATE ${warn_opts})

add_test(NAME image-alignment-test COMMAND image-alignment-test)

add_executable(
  node-field-animation-test
  scene/node_field_animation_test.cpp)

target_link_libraries(node-field-animation-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
target_include_directories(
  node-field-animation-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/animation
  ../backend/scene/internal/scene/include
  ../backend/scene/internal/scenescript
  ../backend/scene/internal/settings
  ../backend/scene/internal/text
  ../common
  ../common/scene
  ../common/fs/include
  ../common/utils/include
  ../../third_party/nlohmann/include)
target_compile_options(node-field-animation-test PRIVATE ${warn_opts})

add_test(NAME node-field-animation-test COMMAND node-field-animation-test)

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
  ../render/vulkan/include
  ${WEVIDEO_GSTREAMER_INCLUDE_DIRS})
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
    dynamic-layer-effect-test
    scene/script/dynamic_layer_effect_test.cpp)

  target_link_libraries(dynamic-layer-effect-test PRIVATE ${PROJECT_NAME} wpSceneBackend)
  target_include_directories(
    dynamic-layer-effect-test PRIVATE
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
    ../render/rendergraph/include
    ../render/vulkan/include
    ../../third_party/nlohmann/include)
  target_compile_options(dynamic-layer-effect-test PRIVATE ${warn_opts} -UNDEBUG)

  add_test(NAME dynamic-layer-effect-test COMMAND dynamic-layer-effect-test)

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
