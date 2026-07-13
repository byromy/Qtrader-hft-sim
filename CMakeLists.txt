cmake_minimum_required(VERSION 3.16)
project(Qtrader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QTRADER_ENABLE_AVX2 "Build the AVX2 layout benchmark" ON)
option(QTRADER_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

add_library(qtrader INTERFACE)
target_include_directories(qtrader INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})

if(QTRADER_ENABLE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

function(qtrader_executable target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE qtrader Threads::Threads)
endfunction()

qtrader_executable(qtrader_orderbook_test tests/PriceTimeOrderBookTest.cpp)
qtrader_executable(qtrader_dense_orderbook_test tests/DenseOrderBookTest.cpp)
qtrader_executable(qtrader_shm_test tests/ShmRingProtocolTest.cpp)
qtrader_executable(qtrader_spsc_bench bench/SpscQueueBench.cpp)
qtrader_executable(qtrader_orderbook_bench bench/DenseOrderBookBench.cpp)
qtrader_executable(qtrader_pipeline_reference src/Level2Pipeline.cpp)
qtrader_executable(qtrader_pipeline_dense src/Level2Pipeline.cpp)
target_compile_definitions(qtrader_pipeline_dense PRIVATE WT_DENSE_ORDERBOOK=1)

if(QTRADER_ENABLE_AVX2)
  qtrader_executable(qtrader_avx2_bench bench/LayoutAvx2Bench.cpp)
  if(MSVC)
    target_compile_options(qtrader_avx2_bench PRIVATE /arch:AVX2)
  else()
    target_compile_options(qtrader_avx2_bench PRIVATE -mavx2)
  endif()
endif()

enable_testing()
add_test(NAME orderbook_reference COMMAND qtrader_orderbook_test)
add_test(NAME orderbook_differential COMMAND qtrader_dense_orderbook_test)
add_test(NAME shm_sequence_ring COMMAND qtrader_shm_test)
