cmake_minimum_required(VERSION 3.24)
project(DisplayMaster VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DM_BUILD_TESTS "Build unit tests" ON)
if(DM_BUILD_TESTS)
  enable_testing()
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

add_subdirectory(protocol)

if(WIN32)
  add_subdirectory(windows/host)
  # Headless protocol client for integration / multi-device tests.
  add_executable(dm_testclient tools/testclient/main.cpp)
  target_link_libraries(dm_testclient PRIVATE dm_protocol ws2_32)
endif()
