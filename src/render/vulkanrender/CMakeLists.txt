set(LIB_NAME wpVulkanRender)

add_library(${LIB_NAME}
STATIC
ClearPass.cpp
CopyPass.cpp
CustomShaderPass.cpp
FinPass.cpp
PrePass.cpp
SceneToRenderGraph.cpp
TextPass.cpp
VulkanPass.cpp
VulkanRender.cpp
)

target_link_libraries(${LIB_NAME}
PRIVATE
	wpVulkan
    wpScene
    wpRGraph
)
target_include_directories(${LIB_NAME}
  PUBLIC include
  PRIVATE
    .
    include/vulkanrender
    ../..
    ../../common/scene
    ../../backend/scene/internal
    ../../backend/scene/internal/animation
    ../../backend/scene/internal/parser
    ../../backend/scene/internal/resources
    ../../backend/scene/internal/scenescript
    ../../backend/scene/internal/settings
    ../../backend/scene/internal/shader
    ../../backend/scene/internal/text
    ../../backend/scene/internal/transform
    ../../backend/scene/internal/scene/include
    ../../backend/scene/internal/scene/include/scene
    ../../backend/scene/internal/particle/include
    ../../backend/scene/internal/particle/include/particle)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts} -Wno-missing-field-initializers)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
