# Web backend is gated on BUILD_WEWEB=ON because WebBackend calls
# into WebBrowserHost (libcef_dll + libcef.so). The default tree has
# no CEF; the main library's BuiltinBackendFactory returns "not
# built" for BackendType::Web in that configuration (see commit 9).
if(BUILD_WEWEB)
    enable_language(ASM)

    set(LIB_NAME wpWebBackend)
    add_library(${LIB_NAME} STATIC
        CreateWebBackend.cpp
        internal/BrowserHostLoader.cpp
        internal/Manifest.cpp
        internal/WebRenderPlan.cpp
        internal/WebBackend.cpp)
    target_link_libraries(${LIB_NAME} PUBLIC
        wpUtils
        nlohmann_json::nlohmann_json)
    target_include_directories(${LIB_NAME} PRIVATE
        ../..    # src/ — for runtime/, output/, common/, host/ includes
        ../../../include)  # project include/ — for wallpaper/ public headers
    target_compile_definitions(${LIB_NAME} PRIVATE
        WP_DEFAULT_CEF_RESOURCES_DIR="${WP_CEF_RESOURCES_DIR}"
        WP_DEFAULT_CEF_LOCALES_DIR="${WP_CEF_LOCALES_DIR}")
    target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
    set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

    set(CEF_LIB_NAME wpWebBrowserHost)
    add_library(${CEF_LIB_NAME} STATIC
        WebBrowserHost.cpp
        internal/DlopenCefBrowserHost.cpp)
    target_link_libraries(${CEF_LIB_NAME} PUBLIC
        wpUtils
        ${CMAKE_DL_LIBS})
    target_include_directories(${CEF_LIB_NAME} PRIVATE
        ../..    # src/ — for backend/... and runtime/... includes
        ../../../include)  # include/ — for wallpaper/web/WebTypes.hpp
    target_compile_options(${CEF_LIB_NAME} PRIVATE ${warn_opts})
    set_property(TARGET ${CEF_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR "we-cef-helper DSO-executable is implemented only for Linux x86_64")
    endif()

    add_library(we-cef-helper SHARED
        WebBrowserHost.cpp
        internal/cef/PropertyBridge.cpp
        internal/cef/helper/dso_start_x86_64.S
        internal/cef/helper/main.cpp
        internal/cef/helper/CefHostExports.cpp
        internal/cef/helper/AppHandler.cpp
        internal/cef/helper/BrowserHost.cpp
        internal/cef/helper/ClientHandler.cpp
        internal/cef/helper/OsrRenderHandler.cpp
        internal/cef/helper/UserProperties.cpp)
    set_target_properties(we-cef-helper PROPERTIES
        PREFIX ""
        SUFFIX ""
        OUTPUT_NAME "we-cef-helper"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
        BUILD_RPATH "${WP_CEF_LIBRARY_DIR}"
        INSTALL_RPATH "${WP_CEF_LIBRARY_DIR}")
    target_link_libraries(we-cef-helper PRIVATE
        wpUtils
        WebCef::Core
        libcef_dll_wrapper)
    target_include_directories(we-cef-helper PRIVATE
        ../..
        ../../../include)
    target_compile_options(we-cef-helper PRIVATE ${warn_opts} -fno-exceptions)
    target_link_options(we-cef-helper PRIVATE
        -Wl,-e,_start
        -Wl,-export-dynamic
        -Wl,-z,noexecstack)
    add_custom_command(TARGET we-cef-helper POST_BUILD
        COMMAND chmod +x $<TARGET_FILE:we-cef-helper>)

    install(TARGETS we-cef-helper
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

    if(CEF_ROOT)
        install(DIRECTORY "${CEF_ROOT}/Release/Resources/"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cef
            OPTIONAL)
    endif()
endif()
