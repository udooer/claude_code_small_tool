cmake_minimum_required(VERSION 3.16)
project(WmfTrainingLabs CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  # 在非 Windows 上只建置平台無關的單元測試
  add_executable(portable_tests tests/portable_tests.cpp)
  target_include_directories(portable_tests PRIVATE common)
  enable_testing()
  add_test(NAME portable_tests COMMAND portable_tests)
  return()
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers)
endif()
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MINGW)
  # 讓產生的 exe 不依賴 mingw 的 runtime DLL，可以直接拷到 Windows 執行
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

add_library(wmf_common STATIC
  common/d3d_util.cpp
  common/desktop_capture.cpp
  common/video_scaler.cpp
  common/h264_encoder.cpp
  common/h264_decoder.cpp
  common/render_window.cpp
  common/pattern_window.cpp
)
target_include_directories(wmf_common PUBLIC common)
target_link_libraries(wmf_common PUBLIC
  d3d11 dxgi mfplat mfuuid wmcodecdspuuid strmiids ole32 oleaut32 user32 gdi32 ws2_32)


add_executable(dda_capture lab1_capture/main.cpp)
add_executable(dda_scale   lab2_scale/main.cpp)
add_executable(dda_nv12    lab3_nv12/main.cpp)
add_executable(dda_encode  lab4_encode/main.cpp)
add_executable(h264_decode lab5_decode/main.cpp)
add_executable(sender      lab6_stream/sender.cpp)
add_executable(receiver    lab6_stream/receiver.cpp)
foreach(t dda_capture dda_scale dda_nv12 dda_encode h264_decode sender receiver)
  target_link_libraries(${t} PRIVATE wmf_common)
endforeach()

add_executable(portable_tests tests/portable_tests.cpp)
target_include_directories(portable_tests PRIVATE common)

# encoder -> decoder 自我測試（不需要螢幕擷取 / GPU）
add_executable(codec_loopback tests/codec_loopback.cpp)
target_link_libraries(codec_loopback PRIVATE wmf_common)
