set(LIB_NAME wpParticle)

add_library(${LIB_NAME}
STATIC
ParticleModify.cpp
ParticleSystem.cpp
ParticleEmitter.cpp
WPParticleRawGener.cpp
)

target_link_libraries(${LIB_NAME} PUBLIC wpUtils PRIVATE wpScene)
target_include_directories(
  ${LIB_NAME}
  PUBLIC include
  PRIVATE
    include/particle
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../common/scene
    ${CMAKE_CURRENT_SOURCE_DIR}/../animation
    ${CMAKE_CURRENT_SOURCE_DIR}/../resources
    ${CMAKE_CURRENT_SOURCE_DIR}/../scenescript
    ${CMAKE_CURRENT_SOURCE_DIR}/../settings
    ${CMAKE_CURRENT_SOURCE_DIR}/../shader
    ${CMAKE_CURRENT_SOURCE_DIR}/../text
    ${CMAKE_CURRENT_SOURCE_DIR}/../transform
    ${CMAKE_CURRENT_SOURCE_DIR}/../scene/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../scene/include/scene)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
