# Shared wire protocol + input math. Built for MSVC (host) and the Android NDK (client).
add_library(dm_protocol STATIC src/protocol.cpp)
target_include_directories(dm_protocol PUBLIC include)
target_compile_features(dm_protocol PUBLIC cxx_std_20)

if(DM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
