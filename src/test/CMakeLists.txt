add_executable(
  wallpaper-session-contract-test
  wallpaper_session_contract_test.cpp)

target_link_libraries(wallpaper-session-contract-test PRIVATE ${PROJECT_NAME})
target_include_directories(wallpaper-session-contract-test PRIVATE ..)
target_compile_options(wallpaper-session-contract-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-session-contract-test COMMAND wallpaper-session-contract-test)

add_executable(
  wallpaper-public-api-test
  wallpaper_public_api_test.cpp)

target_link_libraries(wallpaper-public-api-test PRIVATE ${PROJECT_NAME})
target_compile_options(wallpaper-public-api-test PRIVATE ${warn_opts})

add_test(NAME wallpaper-public-api-test COMMAND wallpaper-public-api-test)

add_executable(
  output-controller-contract-test
  output_controller_contract_test.cpp)

target_link_libraries(output-controller-contract-test PRIVATE ${PROJECT_NAME})
target_include_directories(output-controller-contract-test PRIVATE ..)
target_compile_options(output-controller-contract-test PRIVATE ${warn_opts})

add_test(NAME output-controller-contract-test COMMAND output-controller-contract-test)

add_executable(
  web-backend-contract-test
  web_backend_contract_test.cpp)

target_link_libraries(web-backend-contract-test PRIVATE ${PROJECT_NAME})
target_include_directories(web-backend-contract-test PRIVATE ..)
target_compile_options(web-backend-contract-test PRIVATE ${warn_opts})

add_test(NAME web-backend-contract-test COMMAND web-backend-contract-test)

add_executable(
  host-services-contract-test
  host_services_contract_test.cpp)

target_link_libraries(host-services-contract-test PRIVATE ${PROJECT_NAME})
target_include_directories(host-services-contract-test PRIVATE ..)
target_compile_options(host-services-contract-test PRIVATE ${warn_opts})

add_test(NAME host-services-contract-test COMMAND host-services-contract-test)

add_executable(
  resource-correctness-test
  resource_correctness_test.cpp)

target_link_libraries(resource-correctness-test PRIVATE ${PROJECT_NAME})
target_include_directories(
  resource-correctness-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/scene/include
  ../common
  ../common/fs/include
  ../render/vulkan/include)
target_compile_options(resource-correctness-test PRIVATE ${warn_opts})

add_test(NAME resource-correctness-test COMMAND resource-correctness-test)

add_executable(
  effect-rendergraph-correctness-test
  effect_rendergraph_correctness_test.cpp)

target_link_libraries(effect-rendergraph-correctness-test PRIVATE ${PROJECT_NAME})
target_include_directories(
  effect-rendergraph-correctness-test PRIVATE
  ..
  ../render/rendergraph/include
  ../backend/scene/internal/scene/include
  ../backend/scene/internal
  ../common
  ../common/utils/include
  ../render/vulkan/include)
target_compile_options(effect-rendergraph-correctness-test PRIVATE ${warn_opts})

add_test(NAME effect-rendergraph-correctness-test COMMAND effect-rendergraph-correctness-test)

add_executable(
  particle-regression-test
  particle_regression_test.cpp)

target_link_libraries(particle-regression-test PRIVATE ${PROJECT_NAME})
target_include_directories(
  particle-regression-test PRIVATE
  ..
  ../backend/scene/internal
  ../backend/scene/internal/particle/include
  ../backend/scene/internal/scene/include
  ../common
  ../common/utils/include)
target_compile_options(particle-regression-test PRIVATE ${warn_opts})

add_test(NAME particle-regression-test COMMAND particle-regression-test)

add_test(
  NAME wallpaper-architecture-guard-test
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/architecture_guard_test.cmake)
