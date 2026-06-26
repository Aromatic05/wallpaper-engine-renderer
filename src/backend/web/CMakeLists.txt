set(LIB_NAME wpWebBackend)

add_library(${LIB_NAME} STATIC
    CreateWebBackend.cpp
    internal/Manifest.cpp
    internal/WebBackend.cpp)

target_link_libraries(${LIB_NAME} PUBLIC wpUtils nlohmann_json::nlohmann_json)
target_include_directories(${LIB_NAME} PRIVATE
    ../..    # src/ — for runtime/, output/, common/, host/ includes
    ../../../include)  # project include/ — for wallpaper/ public headers
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

# CEF handler bundle. Only built when BUILD_WEWEB=ON — these source
# files include <cef_*.h> and link against libcef_dll_wrapper, so
# neither is available in the default tree. The target is
# intentionally not linked into the main library yet; commit 5 wires
# it through once BrowserHost consumes these handlers.
if(BUILD_WEWEB)
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
    target_compile_definitions(${CEF_LIB_NAME} PRIVATE
        # CEF's libcef_dll wrapper is built without exceptions.
        -fno-exceptions)
    set_property(TARGET ${CEF_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
