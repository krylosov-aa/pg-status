add_library(utils utils.c)

pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

target_link_libraries(utils PUBLIC common_warnings PkgConfig::CJSON)

target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
