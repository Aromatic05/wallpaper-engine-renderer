add_subdirectory(internal)

set(LIB_NAME wpSceneBackendModule)

add_library(${LIB_NAME} STATIC CreateWESceneBackend.cpp)

target_link_libraries(${LIB_NAME} PUBLIC wpUtils PRIVATE wpSceneBackend)
target_include_directories(${LIB_NAME} PRIVATE ../..)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
