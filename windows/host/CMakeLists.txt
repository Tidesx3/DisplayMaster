# DisplayMaster host engine (Windows).
add_library(dm_host_core STATIC
  src/core/log.cpp
  src/core/win.cpp
  src/core/config.cpp
  src/display/display_config.cpp
  src/display/virtual_display.cpp
  src/capture/dda_capture.cpp
  src/capture/video_converter.cpp
  src/capture/cursor_compositor.cpp
  src/capture/shared_capture.cpp
  src/encode/encoder_factory.cpp
  src/encode/nvenc_encoder.cpp
  src/encode/amf_encoder.cpp
  src/transport/connection.cpp
  src/transport/adb.cpp
  src/transport/mdns.cpp
  src/input/input_injector.cpp
  src/session/video_pipeline.cpp
  src/session/session.cpp
  src/session/host.cpp
  src/session/approval.cpp
  src/ipc/control_server.cpp
  src/setup/vdd_setup.cpp
)
target_include_directories(dm_host_core
  PUBLIC src
  PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nvenc ${PROJECT_SOURCE_DIR}/third_party/amf/include)
target_link_libraries(dm_host_core PUBLIC dm_protocol d3d11 dxgi d3dcompiler ws2_32 iphlpapi advapi32 dnsapi shell32 ole32 setupapi newdev user32)
# Synthetic pointer API needs the Windows 10 1809 headers.
target_compile_definitions(dm_host_core PUBLIC _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)

# GUI subsystem: started by the app or its task, the engine runs without a console window.
# main.cpp attaches to the parent console when run from a terminal (diagnostics, -v).
add_executable(DisplayMasterHost WIN32 src/main.cpp src/diagnostics.cpp host.manifest)
target_link_libraries(DisplayMasterHost PRIVATE dm_host_core)
target_link_options(DisplayMasterHost PRIVATE /ENTRY:wmainCRTStartup)

if(DM_BUILD_TESTS)
  add_executable(dm_host_tests tests/virtual_display_test.cpp tests/cursor_test.cpp tests/json_test.cpp tests/approval_test.cpp)
  target_link_libraries(dm_host_tests PRIVATE dm_host_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(dm_host_tests)
endif()
# Our manifest carries the UAC settings.
target_link_options(DisplayMasterHost PRIVATE /MANIFESTUAC:NO)
