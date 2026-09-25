cmake_minimum_required(VERSION 3.22.1)
project(displaymaster_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared wire protocol (same sources as the Windows host).
set(DM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(DM_BUILD_TESTS OFF)
add_subdirectory(${DM_ROOT}/protocol ${CMAKE_BINARY_DIR}/protocol)

add_library(dmclient SHARED
  client.cpp
  decoder.cpp
  jni_bridge.cpp)
target_compile_options(dmclient PRIVATE -Wall -Wextra -O2)
# Newer NDK APIs are weak-linked and guarded with __builtin_available (minSdk is 24).
target_compile_definitions(dmclient PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_link_libraries(dmclient PRIVATE dm_protocol mediandk android log)
# If the SDK path contains a space, CMake invokes clang++ by its 8.3 short name
# (CLANG_~1.EXE); clang then can't tell from argv[0] that it should link as C++
# and leaves out libc++. Force C++ driver mode explicitly.
target_link_options(dmclient PRIVATE --driver-mode=g++)
