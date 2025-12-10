add_library(http_server http_server.c)


target_link_libraries(http_server PUBLIC utils)

find_package(PkgConfig REQUIRED)

pkg_check_modules(MICROHTTPD REQUIRED libmicrohttpd)

target_include_directories(http_server
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(http_server PUBLIC ${MICROHTTPD_INCLUDE_DIRS})
target_link_directories(http_server PUBLIC ${MICROHTTPD_LIBRARY_DIRS})
target_link_libraries(http_server PUBLIC ${MICROHTTPD_LIBRARIES})
