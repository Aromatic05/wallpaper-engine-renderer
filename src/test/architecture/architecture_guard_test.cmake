file(GLOB_RECURSE public_headers "${PROJECT_SOURCE_DIR}/include/wallpaper/*.hpp")

set(public_boundary_violations "")
foreach(header IN LISTS public_headers)
  file(READ "${header}" header_content)
  if(header_content MATCHES "\\.\\./\\.\\./src/" OR header_content MATCHES "\\.\\./\\.\\./\\.\\./src/")
    list(APPEND public_boundary_violations "${header}")
  endif()

  if(header_content MATCHES "OutputSourceType" OR
     header_content MATCHES "supportsRenderPlan" OR
     header_content MATCHES "supportsTextureOutput" OR
     header_content MATCHES "supportsSurfaceOutput")
    list(APPEND public_boundary_violations "${header} (declares output source branching outside the mandatory RenderPlan contract)")
  endif()
endforeach()

if(public_boundary_violations)
  string(REPLACE ";" "\n" violation_report "${public_boundary_violations}")
  message(FATAL_ERROR "public wallpaper headers must not include src/ internals:\n${violation_report}")
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/src/backend/scene/internal/runtime")
  message(FATAL_ERROR "scene backend internal/runtime directory must not exist")
endif()

foreach(placeholder IN ITEMS
    "${PROJECT_SOURCE_DIR}/src/output/TextureSource.hpp"
    "${PROJECT_SOURCE_DIR}/src/output/SurfaceSource.hpp")
  if(EXISTS "${placeholder}")
    message(FATAL_ERROR "unimplemented output source placeholder must not exist: ${placeholder}")
  endif()
endforeach()