cmake_minimum_required(VERSION 3.16)
project(OvalFFR VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(LAYER_NAME XR_APILAYER_OVALFFR_oval_ffr)

set(CORE_SRC src/ovalffr_core.cpp src/ovalffr_gpu.cpp src/ovalffr_dyn.cpp src/ovalffr_smooth.cpp)

# The smoothing shader is kept as a normal .hlsl file and embedded as a C string at configure time.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS shaders/smooth.hlsl)
file(READ shaders/smooth.hlsl OVALFFR_HLSL)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/smooth_hlsl.h
     "// generated from layer/shaders/smooth.hlsl - do not edit\n#pragma once\nstatic const char kSmoothHlsl[] = R\"OVALHLSL(${OVALFFR_HLSL})OVALHLSL\";\n")
set(LAYER_SRC src/layer.cpp src/backend_null.cpp src/backend_d3d12.cpp ${CORE_SRC})
# gpu_dxgi.cpp is added below for Windows only

add_library(${LAYER_NAME} SHARED ${LAYER_SRC})
if(NOT MSVC)
  target_compile_options(${LAYER_NAME} PRIVATE -Wall -Wextra)
endif()
target_include_directories(${LAYER_NAME} PRIVATE include src ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_include_directories(${LAYER_NAME} SYSTEM PRIVATE third_party/OpenXR-SDK/include)

if(WIN32)
  # DXGI adapter listing lives in its own object library so it sees the SYSTEM dxgi headers,
  # never the vendored DirectX-Headers directory (the two define the same types).
  add_library(ovalffr_dxgi OBJECT src/gpu_dxgi.cpp)
  target_include_directories(ovalffr_dxgi PRIVATE include)
  target_sources(${LAYER_NAME} PRIVATE $<TARGET_OBJECTS:ovalffr_dxgi>)
  target_link_libraries(${LAYER_NAME} PRIVATE dxgi user32)
endif()

if(MINGW)
  # mingw-w64's own d3d12.h predates Variable Rate Shading; use Microsoft's headers.
  target_include_directories(${LAYER_NAME} BEFORE PRIVATE third_party/DirectX-Headers/include/directx)
  target_link_options(${LAYER_NAME} PRIVATE -static-libgcc -static-libstdc++ -static)
  set_target_properties(${LAYER_NAME} PROPERTIES PREFIX "")
elseif(WIN32)
  target_compile_definitions(${LAYER_NAME} PRIVATE NOMINMAX)
else()
  set_target_properties(${LAYER_NAME} PROPERTIES PREFIX "lib")
endif()

# ---- tests (native builds only) ----------------------------------------------
if(NOT CMAKE_CROSSCOMPILING AND NOT WIN32)
  enable_testing()
  add_executable(core_test ../tests/core_test.cpp ${CORE_SRC})
  target_include_directories(core_test PRIVATE include src)
  add_test(NAME core COMMAND core_test)

  add_executable(fuzz_test ../tests/fuzz_test.cpp ${CORE_SRC})
  target_include_directories(fuzz_test PRIVATE include src)
  add_test(NAME fuzz COMMAND fuzz_test)

  add_executable(smooth_test ../tests/smooth_test.cpp ${CORE_SRC})
  target_include_directories(smooth_test PRIVATE include src)
  add_test(NAME smooth COMMAND smooth_test)

  add_executable(smooth_demo ../tests/smooth_demo.cpp ${CORE_SRC})
  target_include_directories(smooth_demo PRIVATE include src)

  add_executable(overlay_demo ../tests/overlay_demo.cpp ${CORE_SRC})
  target_include_directories(overlay_demo PRIVATE include src)

  add_executable(mask_dump ../tests/mask_dump.cpp ${CORE_SRC})
  target_include_directories(mask_dump PRIVATE include)

  add_executable(layer_test ../tests/layer_test.cpp ${CORE_SRC})
  target_include_directories(layer_test PRIVATE include)
  target_include_directories(layer_test SYSTEM PRIVATE third_party/OpenXR-SDK/include)
  target_link_libraries(layer_test PRIVATE ${CMAKE_DL_LIBS})
  add_dependencies(layer_test ${LAYER_NAME})
endif()
