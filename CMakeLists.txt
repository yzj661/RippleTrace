cmake_minimum_required(VERSION 3.16)
project(RippleTrace CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB RIPPLETRACE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(RippleTrace ${RIPPLETRACE_SOURCES})
target_include_directories(RippleTrace PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
  target_compile_options(RippleTrace PRIVATE /W3 /utf-8 /EHsc)
  target_compile_definitions(RippleTrace PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(RippleTrace PRIVATE -Wall -Wextra -O2)
endif()

install(TARGETS RippleTrace RUNTIME DESTINATION bin)
