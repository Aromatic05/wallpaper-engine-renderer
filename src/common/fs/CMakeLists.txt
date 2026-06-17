set(LIB_NAME wpFs)

add_library(${LIB_NAME} INTERFACE)

target_link_libraries(${LIB_NAME} INTERFACE wpUtils)
target_include_directories(${LIB_NAME} INTERFACE include)
