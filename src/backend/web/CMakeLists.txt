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
