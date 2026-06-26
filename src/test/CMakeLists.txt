add_executable(
  wallpaper-session-contract-test
  wallpaper_session_contract_test.cpp)

target_link_libraries(wallpaper-session-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(wallpaper-session-contract-test PRIVATE ..)
target_compile_options(wallpaper-session-contract-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-session-contract-test COMMAND wallpaper-session-contract-test)

add_executable(
  wallpaper-public-api-test
  wallpaper_public_api_test.cpp)

target_link_libraries(wallpaper-public-api-test PRIVATE wallpaper-engine-renderer-internal)
target_compile_options(wallpaper-public-api-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-public-api-test COMMAND wallpaper-public-api-test)

add_executable(
  output-controller-contract-test
  output_controller_contract_test.cpp)

target_link_libraries(output-controller-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(output-controller-contract-test PRIVATE ..)
target_compile_options(output-controller-contract-test PRIVATE ${warn_opts})

add_test(NAME output-controller-contract-test COMMAND output-controller-contract-test)

add_executable(
  web-backend-contract-test
  web_backend_contract_test.cpp)

target_link_libraries(web-backend-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(web-backend-contract-test PRIVATE ..)
target_compile_options(web-backend-contract-test PRIVATE ${warn_opts})

add_test(NAME web-backend-contract-test COMMAND web-backend-contract-test)

add_executable(
  host-services-contract-test
  host_services_contract_test.cpp)

target_link_libraries(host-services-contract-test PRIVATE wallpaper-engine-renderer-internal)
target_include_directories(host-services-contract-test PRIVATE ..)
target_compile_options(host-services-contract-test PRIVATE ${warn_opts})

add_test(NAME host-services-contract-test COMMAND host-services-contract-test)

add_executable(
  resource-correctness-test
  resource_correctness_test.cpp)

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
  ../render/vulkan/include)
target_compile_options(resource-correctness-test PRIVATE ${warn_opts})

add_test(NAME resource-correctness-test COMMAND resource-correctness-test)

add_executable(
  effect-rendergraph-correctness-test
  effect_rendergraph_correctness_test.cpp)

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
  particle_regression_test.cpp)

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
  synthetic_image_parser_test.cpp)

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
  scene_parity_foundation_test.cpp)

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
  node_transform_resolver_test.cpp)

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
  ../common/utils/include)
target_compile_options(node-transform-resolver-test PRIVATE ${warn_opts})

add_test(NAME node-transform-resolver-test COMMAND node-transform-resolver-test)

add_executable(
  text-scene-model-test
  text_scene_model_test.cpp)

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
  text_layer_runtime_test.cpp)

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
  text_clear_pass_test.cpp)

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
  ../common/scene)
target_compile_options(text-clear-pass-test PRIVATE ${warn_opts})

add_test(NAME text-clear-pass-test COMMAND text-clear-pass-test)

add_executable(
  video-texture-cache-test
  video_texture_cache_test.cpp)

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
  migrated_features_regression_test.cpp)

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
  dynamic_value_user_setting_test.cpp)

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
  property_animation_test.cpp)

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
    scenescript_host_test.cpp)

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
  target_compile_options(scenescript-host-test PRIVATE ${warn_opts})

  add_test(NAME scenescript-host-test COMMAND scenescript-host-test)

  add_executable(
    scenescript-runtime-test
    scenescript_runtime_test.cpp)

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
          -P ${CMAKE_CURRENT_SOURCE_DIR}/architecture_guard_test.cmake)
