cmake_minimum_required(VERSION 3.16)
project(QtraderComponents LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
enable_testing()

add_library(qtrader_components INTERFACE)
target_include_directories(qtrader_components INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

foreach(test IN ITEMS DenseOrderBook PriceTimeOrderBook ShmRingProtocol SpscRing)
  add_executable(component_${test}_test tests/${test}Test.cpp)
  target_link_libraries(component_${test}_test PRIVATE qtrader_components Threads::Threads)
  if(MSVC)
    target_compile_options(component_${test}_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(component_${test}_test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME ${test} COMMAND component_${test}_test)
endforeach()
