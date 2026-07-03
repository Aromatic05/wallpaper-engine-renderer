if(BUILD_WEVIDEO)
    set(LIB_NAME wpVideoBackend)

    pkg_check_modules(GSTREAMER REQUIRED
        gstreamer-1.0
        gstreamer-app-1.0
        gstreamer-allocators-1.0
        gstreamer-video-1.0
        gstreamer-va-1.0
        libdrm)

    add_library(${LIB_NAME} STATIC
        CreateVideoBackend.cpp
        internal/VideoBackend.cpp
        internal/VideoRenderPlan.cpp)

    target_link_libraries(${LIB_NAME} PUBLIC
        wpUtils
        nlohmann_json::nlohmann_json
        ${GSTREAMER_LIBRARIES})
    target_include_directories(${LIB_NAME} PRIVATE
        ../..
        ../../../include
        ${GSTREAMER_INCLUDE_DIRS})
    target_compile_definitions(${LIB_NAME} PRIVATE GST_USE_UNSTABLE_API)
    target_compile_options(${LIB_NAME} PRIVATE ${warn_opts} ${GSTREAMER_CFLAGS_OTHER})
    set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
