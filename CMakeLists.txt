cmake_minimum_required(VERSION 3.16)
project(Qtrader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

if(UNIX AND NOT APPLE)
  qtrader_executable(qtrader_udp_loopback_bench bench/UdpLoopbackBench.cpp)
  target_compile_definitions(qtrader_udp_loopback_bench PRIVATE _GNU_SOURCE)
  qtrader_executable(qtrader_itch50_moldudp_loopback tools/NasdaqMoldUdpLoopback.cpp)
endif()
qtrader_executable(qtrader_itch50_inspect tools/NasdaqItchInspect.cpp)
qtrader_executable(qtrader_itch50_wts_replay tools/NasdaqItchWtsReplay.cpp)
qtrader_executable(qtrader_itch50_uft_differential_replay tools/NasdaqItchUftDifferentialReplay.cpp)
qtrader_executable(qtrader_itch50_test tests/NasdaqItch50Test.cpp)
qtrader_executable(qtrader_moldudp64_test tests/MoldUdp64Test.cpp)
qtrader_executable(qtrader_moldudp64_recovery_test tests/MoldUdp64RecoveryTest.cpp)
qtrader_executable(qtrader_wts_fingerprint_test tests/WtsEventFingerprintTest.cpp)
qtrader_executable(qtrader_uft_market_data_test tests/UftMarketDataEngineTest.cpp)
qtrader_executable(qtrader_fixed_hash_table_test tests/FixedHashTableTest.cpp)
qtrader_executable(qtrader_itch50_uft_adapter_test tests/NasdaqItchUftAdapterTest.cpp)

set(QTRADER_TEST_TARGETS
  qtrader_itch50_test
  qtrader_moldudp64_test
  qtrader_moldudp64_recovery_test
  qtrader_wts_fingerprint_test
  qtrader_uft_market_data_test
  qtrader_fixed_hash_table_test
  qtrader_itch50_uft_adapter_test)
foreach(target IN LISTS QTRADER_TEST_TARGETS)
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endforeach()

enable_testing()
add_test(NAME nasdaq_itch50_primitives COMMAND qtrader_itch50_test)
add_test(NAME moldudp64_protocol COMMAND qtrader_moldudp64_test)
add_test(NAME moldudp64_recovery COMMAND qtrader_moldudp64_recovery_test)
add_test(NAME wts_event_fingerprint COMMAND qtrader_wts_fingerprint_test)
add_test(NAME uft_market_data_engine COMMAND qtrader_uft_market_data_test)
add_test(NAME fixed_hash_table COMMAND qtrader_fixed_hash_table_test)
add_test(NAME nasdaq_itch50_uft_adapter COMMAND qtrader_itch50_uft_adapter_test)
