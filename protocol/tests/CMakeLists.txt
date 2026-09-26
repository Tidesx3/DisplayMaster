include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)  # static CRT, like the rest of the build
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(dm_protocol_tests protocol_test.cpp input_math_test.cpp noise_test.cpp udp_video_test.cpp)
target_link_libraries(dm_protocol_tests PRIVATE dm_protocol GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(dm_protocol_tests)
