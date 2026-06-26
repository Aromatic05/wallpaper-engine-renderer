# Web backend is gated on BUILD_WEWEB=ON because WebBackend calls
# into WebBrowserHost (libcef_dll + libcef.so). The default tree has
# no CEF; the main library's BuiltinBackendFactory returns "not
# built" for BackendType::Web in that configuration (see commit 9).
if(BUILD_WEWEB)
    set(LIB_NAME wpWebBackend)
    add_library(${LIB_NAME} STATIC
        CreateWebBackend.cpp
        internal/Manifest.cpp
        internal/WebRenderPlan.cpp
        internal/WebBackend.cpp)
    target_link_libraries(${LIB_NAME} PUBLIC
        wpUtils
        wpWebBrowserHost
        nlohmann_json::nlohmann_json)
    target_include_directories(${LIB_NAME} PRIVATE
        ../..    # src/ — for runtime/, output/, common/, host/ includes
        ../../../include)  # project include/ — for wallpaper/ public headers
    target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
    set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

    set(CEF_LIB_NAME wpWebBrowserHost)
    add_library(${CEF_LIB_NAME} STATIC
        internal/cef/AppHandler.cpp
        internal/cef/BrowserHost.cpp
        internal/cef/ClientHandler.cpp
        internal/cef/OsrRenderHandler.cpp
        internal/cef/UserProperties.cpp)
    target_link_libraries(${CEF_LIB_NAME} PUBLIC
        wpUtils
        WebCef::Core
        libcef_dll_wrapper)
    target_include_directories(${CEF_LIB_NAME} PRIVATE
        ../..    # src/ — for backend/... and runtime/... includes
        ../../../include)  # include/ — for wallpaper/web/WebTypes.hpp
    target_compile_options(${CEF_LIB_NAME} PRIVATE ${warn_opts})
    target_compile_options(${CEF_LIB_NAME} PRIVATE
        # CEF's libcef_dll wrapper is built without exceptions.
        -fno-exceptions)
    set_property(TARGET ${CEF_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

    add_executable(we-cef-helper
        internal/cef/helper/main.cpp
        internal/cef/AppHandler.cpp
        internal/cef/UserProperties.cpp)
    target_link_libraries(we-cef-helper PRIVATE
        WebCef::Core
        libcef_dll_wrapper)
    target_include_directories(we-cef-helper PRIVATE
        ../..
        ../../../include)
    target_compile_options(we-cef-helper PRIVATE ${warn_opts} -fno-exceptions)

    install(TARGETS we-cef-helper
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/wallpaper-engine-renderer)

    if(CEF_ROOT)
        install(DIRECTORY "${CEF_ROOT}/Release/Resources/"
            DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/wallpaper-engine-renderer/cef
            OPTIONAL)
    endif()
endif()
