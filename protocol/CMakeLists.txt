# Shared wire protocol + input math. Built for MSVC (host) and the Android NDK (client).
# Monocypher (third_party, BSD-2/CC0) is C99 that also compiles as C++: built as C++ so
# both toolchains only need their C++ compiler.
set(DM_MONOCYPHER ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher)
set_source_files_properties(${DM_MONOCYPHER}/monocypher.c PROPERTIES LANGUAGE CXX)
if(MSVC)
  set_source_files_properties(${DM_MONOCYPHER}/monocypher.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(${DM_MONOCYPHER}/monocypher.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(dm_protocol STATIC src/protocol.cpp src/noise.cpp ${DM_MONOCYPHER}/monocypher.c)
target_include_directories(dm_protocol PUBLIC include ${DM_MONOCYPHER})
if(WIN32)
  target_link_libraries(dm_protocol PUBLIC bcrypt)
endif()
target_compile_features(dm_protocol PUBLIC cxx_std_20)

if(DM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
