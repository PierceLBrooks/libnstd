cmake_minimum_required(VERSION 3.1)

project(libnstd)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

if(MSVC)
    option(UNICODE "Build with unicode character set" OFF)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER ".cmake")

enable_testing()

add_subdirectory(src)
add_subdirectory(test)
