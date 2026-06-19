set(LIB_NAME wpVulkan)

find_package(Vulkan REQUIRED)
include(CheckIncludeFileCXX)
check_include_file_cxx(cuda.h HANABI_HAS_CUDA_HEADER)
pkg_check_modules(GSTREAMER REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-allocators-1.0
    gstreamer-video-1.0
    gstreamer-va-1.0
    gstreamer-cuda-1.0
    libdrm
    libva
    gio-2.0)

add_library(${LIB_NAME}
STATIC
Instance.cpp
Device.cpp
GraphicsPipeline.cpp
Shader.cpp
StagingBuffer.cpp
Swapchain.cpp
TextureCache.cpp
VideoTextureCache.cpp
Parameters.cpp
Vma.cpp
vulkan_wrapper.cpp
)

target_link_libraries(${LIB_NAME}
PUBLIC
	glslang
    spirv-reflect-static
	${Vulkan_LIBRARIES}
    wpScene
PRIVATE
	SPIRV
    ${GSTREAMER_LIBRARIES}
)
target_include_directories(${LIB_NAME}
  PUBLIC include
         ../../../include
  PRIVATE
    include/vulkan
    include/vkk
    ../../common/scene
    ${GSTREAMER_INCLUDE_DIRS})
target_compile_definitions(${LIB_NAME} PRIVATE GST_USE_UNSTABLE_API)
if(HANABI_HAS_CUDA_HEADER)
  target_compile_definitions(${LIB_NAME} PRIVATE HANABI_HAS_CUDA_INTEROP=1)
endif()
if(HANABI_HAS_DXC)
  target_compile_definitions(${LIB_NAME} PRIVATE HANABI_HAS_DXC=1)
  target_link_libraries(${LIB_NAME} PRIVATE Hanabi::DxCompiler)
  if(TARGET HanabiVendoredDxc)
    add_dependencies(${LIB_NAME} HanabiVendoredDxc)
  endif()
endif()
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts} -Wno-missing-field-initializers ${GSTREAMER_CFLAGS_OTHER})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
