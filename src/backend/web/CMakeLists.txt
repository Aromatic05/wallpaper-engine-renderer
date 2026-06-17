set(LIB_NAME wpWebBackend)

add_library(${LIB_NAME} STATIC CreateWebBackend.cpp internal/WebBackend.cpp)

target_link_libraries(${LIB_NAME} PUBLIC wpUtils)
target_include_directories(${LIB_NAME} PRIVATE ../..)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
