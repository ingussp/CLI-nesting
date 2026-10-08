cmake_minimum_required(VERSION 3.20)
project(clinesting LANGUAGES CXX)

option(CLINESTING_STATIC_RUNTIME "Use the static MSVC runtime for a standalone executable" ON)
if(MSVC AND CLINESTING_STATIC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
find_package(Threads REQUIRED)

set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  clipper2
  GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
  GIT_TAG Clipper2_1.5.4
  SOURCE_SUBDIR CPP
)
FetchContent_MakeAvailable(clipper2)

add_library(clinesting
  src/bitmap_nesting.cpp
  src/continuous_nesting.cpp
  src/gpu_bitmap.cpp
  src/demo_cli.cpp
  src/demo_setup.cpp
  src/dxf_export.cpp
  src/geometry.cpp
  src/nfp_cache.cpp
  src/nfp.cpp
  src/orchestrator.cpp
  src/json_io.cpp
)

target_include_directories(clinesting
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencl
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(clinesting PUBLIC Clipper2 Threads::Threads ${CMAKE_DL_LIBS})

add_executable(clinesting_demo demo/main.cpp)
target_link_libraries(clinesting_demo PRIVATE clinesting)

add_executable(clinesting_cli demo/json_main.cpp)
set_target_properties(clinesting_cli PROPERTIES OUTPUT_NAME clinesting)
target_link_libraries(clinesting_cli PRIVATE clinesting)

option(CLINESTING_BUILD_TESTS "Build nesting regression tests" OFF)
if(CLINESTING_BUILD_TESTS)
  enable_testing()
  add_executable(clinesting_tests tests/regression.cpp)
  target_include_directories(clinesting_tests PRIVATE src)
  target_link_libraries(clinesting_tests PRIVATE clinesting)
  add_test(NAME nesting_regression COMMAND clinesting_tests)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME workbench_protocol
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/workbench_protocol.py
              --exe $<TARGET_FILE:clinesting_cli>)
  endif()
endif()


if(CLINESTING_BUILD_TESTS)
  add_executable(clearance_index_tests tests/clearance_index.cpp)
  target_include_directories(clearance_index_tests PRIVATE src)
  target_link_libraries(clearance_index_tests PRIVATE clinesting)
  add_test(NAME clearance_index COMMAND clearance_index_tests)
endif()

if(CLINESTING_BUILD_TESTS)
  add_executable(validate_indexed_layout tests/validate_indexed_layout.cpp)
  target_include_directories(validate_indexed_layout PRIVATE third_party)
  target_link_libraries(validate_indexed_layout PRIVATE clinesting)
  if(Python3_Interpreter_FOUND)
    add_test(NAME quality_examples
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality_examples.py
              --exe $<TARGET_FILE:clinesting_cli> --validator $<TARGET_FILE:validate_indexed_layout>)
  endif()
endif()

if(CLINESTING_BUILD_TESTS)
  add_executable(convex_pair_cache_tests tests/convex_pair_cache.cpp)
  target_include_directories(convex_pair_cache_tests PRIVATE src)
  target_link_libraries(convex_pair_cache_tests PRIVATE clinesting)
  add_test(NAME convex_pair_cache COMMAND convex_pair_cache_tests)
endif()

if(CLINESTING_BUILD_TESTS)
  add_executable(repair_saved_layout tests/repair_saved_layout.cpp)
  target_include_directories(repair_saved_layout PRIVATE third_party)
  target_link_libraries(repair_saved_layout PRIVATE clinesting)
endif()
