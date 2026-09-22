set(SOURCES "")
file(GLOB MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/*.c
)
list(APPEND SOURCES ${MAIN_SOURCES})
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
)
