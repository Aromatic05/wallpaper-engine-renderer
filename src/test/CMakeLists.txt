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

add_test(
  NAME wallpaper-architecture-guard-test
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/architecture_guard_test.cmake)
