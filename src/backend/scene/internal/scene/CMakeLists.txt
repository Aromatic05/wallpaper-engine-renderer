set(LIB_NAME wpScene)

add_library(${LIB_NAME}
STATIC

Scene.cpp
SceneCamera.cpp
SceneImageEffectLayer.cpp
SceneIndexArray.cpp
SceneNode.cpp
SceneVertexArray.cpp
SceneShader.cpp
)

target_link_libraries(${LIB_NAME}
PUBLIC
    wpUtils
    nlohmann_json
PRIVATE
    wpFs
    wpParticle
)
target_include_directories(
  ${LIB_NAME}
  PUBLIC include
  PRIVATE
    include/scene
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../common/scene
    ${CMAKE_CURRENT_SOURCE_DIR}/../animation
    ${CMAKE_CURRENT_SOURCE_DIR}/../resources
    ${CMAKE_CURRENT_SOURCE_DIR}/../scenescript
    ${CMAKE_CURRENT_SOURCE_DIR}/../settings
    ${CMAKE_CURRENT_SOURCE_DIR}/../shader
    ${CMAKE_CURRENT_SOURCE_DIR}/../text
    ${CMAKE_CURRENT_SOURCE_DIR}/../transform
    ${CMAKE_CURRENT_SOURCE_DIR}/../particle/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../particle/include/particle)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
