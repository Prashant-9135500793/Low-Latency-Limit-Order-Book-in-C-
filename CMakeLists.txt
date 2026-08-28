cmake_minimum_required(VERSION 3.20)
project(low_latency_lob VERSION 2.0.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(LOB_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(LOB_ENABLE_LTO "Enable interprocedural optimization in supported release builds" OFF)
option(LOB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LOB_BUILD_BENCHMARKS "Build benchmark executables" ON)
option(LOB_BUILD_FUZZERS "Build the libFuzzer matching-engine target (Clang only)" OFF)

# Backward-compatible alias used by the original project documentation.
option(ENABLE_SANITIZERS "Deprecated alias for LOB_ENABLE_ASAN_UBSAN" OFF)
if(ENABLE_SANITIZERS)
  set(LOB_ENABLE_ASAN_UBSAN ON CACHE BOOL "" FORCE)
endif()

if(LOB_ENABLE_ASAN_UBSAN AND LOB_ENABLE_TSAN)
  message(FATAL_ERROR "ASan/UBSan and TSan must use separate build directories")
endif()

find_package(Threads REQUIRED)

add_library(lob_project_options INTERFACE)
target_compile_features(lob_project_options INTERFACE cxx_std_20)

target_compile_options(lob_project_options INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
  $<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
  $<$<BOOL:${LOB_WARNINGS_AS_ERRORS}>:-Werror>
)

if(LOB_ENABLE_ASAN_UBSAN)
  target_compile_options(lob_project_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_project_options INTERFACE -fsanitize=address,undefined)
endif()

if(LOB_ENABLE_TSAN)
  target_compile_options(lob_project_options INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(lob_project_options INTERFACE -fsanitize=thread)
endif()

if(LOB_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT LOB_IPO_SUPPORTED OUTPUT LOB_IPO_ERROR)
  if(LOB_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  else()
    message(WARNING "LTO requested but unsupported: ${LOB_IPO_ERROR}")
  endif()
endif()

add_library(lob_core STATIC
  src/execution_report.cpp
  src/journal.cpp
  src/matching_engine.cpp
  src/order_book.cpp
  src/risk_manager.cpp
  src/shared_memory.cpp
  src/timestamp.cpp
)
target_include_directories(lob_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(lob_core PUBLIC Threads::Threads lob_project_options)

function(lob_add_app target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE lob_core)
endfunction()

function(lob_add_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE lob_core)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

function(lob_add_benchmark target source)
  if(LOB_BUILD_BENCHMARKS)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE lob_core)
  endif()
endfunction()

# Applications.
lob_add_app(feed_handler apps/feed_handler.cpp)
lob_add_app(matching_engine_main apps/matching_engine_main.cpp)
lob_add_app(market_viewer apps/market_viewer.cpp)
lob_add_app(cleanup_shared_memory apps/cleanup_shared_memory.cpp)
lob_add_app(mpmc_matching_main apps/mpmc_matching_main.cpp)
lob_add_app(partitioned_matching_main apps/partitioned_matching_main.cpp)
lob_add_app(journal_replay_main apps/journal_replay_main.cpp)

# Unit, integration, concurrency, persistence, and randomized tests.
lob_add_test(test_order_book tests/test_order_book.cpp)
lob_add_test(test_matching tests/test_matching.cpp)
lob_add_test(test_advanced_orders tests/test_advanced_orders.cpp)
lob_add_test(test_journal tests/test_journal.cpp)
lob_add_test(test_spsc tests/test_spsc.cpp)
lob_add_test(test_ipc tests/test_ipc.cpp)
lob_add_test(test_mpmc tests/test_mpmc.cpp)
lob_add_test(test_partitioned_engine tests/test_partitioned_engine.cpp)
lob_add_test(test_randomized_matching tests/test_randomized_matching.cpp)

# Microbenchmarks and end-to-end pipeline benchmarks.
lob_add_benchmark(benchmark_order_book benchmarks/benchmark_order_book.cpp)
lob_add_benchmark(benchmark_spsc benchmarks/benchmark_spsc.cpp)
lob_add_benchmark(benchmark_ipc benchmarks/benchmark_ipc.cpp)
lob_add_benchmark(benchmark_partitioned benchmarks/benchmark_partitioned.cpp)

if(LOB_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LOB_BUILD_FUZZERS requires Clang/libFuzzer")
  endif()
  add_executable(fuzz_matching_engine fuzz/fuzz_matching_engine.cpp)
  target_link_libraries(fuzz_matching_engine PRIVATE lob_core)
  target_compile_options(fuzz_matching_engine PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_matching_engine PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(TARGETS lob_core
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS
  feed_handler matching_engine_main market_viewer cleanup_shared_memory
  mpmc_matching_main partitioned_matching_main journal_replay_main
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lob)
