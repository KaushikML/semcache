cmake_minimum_required(VERSION 3.20)
project(semcache VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SEMCACHE_WERROR "Treat warnings as errors" ON)
option(SEMCACHE_BUILD_TESTS "Build tests" ON)
option(SEMCACHE_BUILD_BENCH "Build benchmarks" ON)
set(SEMCACHE_SANITIZE "" CACHE STRING "Sanitizers: '', 'address,undefined', or 'thread'")

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/onnxruntime.cmake)

find_package(Boost CONFIG REQUIRED COMPONENTS url)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(utf8proc CONFIG REQUIRED)

add_library(semcache_core STATIC
  src/common/sha256.cpp
  src/common/worker_pool.cpp
  src/cache/cache_store.cpp
  src/cache/completion_codec.cpp
  src/cache/context_key.cpp
  src/index/simd.cpp
  src/index/brute_force_index.cpp
  src/config/config.cpp
  src/embed/bert_normalizer.cpp
  src/embed/wordpiece_tokenizer.cpp
  src/embed/onnx_embedder.cpp
  src/metrics/metrics.cpp
  src/log/log.cpp
  src/auth/tenants.cpp
  src/limits/rate_limiter.cpp
  src/net/listener.cpp
  src/http/errors.cpp
  src/http/beast_response_writer.cpp
  src/upstream/http_upstream.cpp
  src/proxy/pipeline.cpp
  src/proxy/proxy_server.cpp
)
target_include_directories(semcache_core PUBLIC src)
target_compile_definitions(semcache_core PUBLIC BOOST_ASIO_NO_DEPRECATED BOOST_BEAST_USE_STD_STRING_VIEW)
target_link_libraries(semcache_core PUBLIC
  Boost::headers Boost::url OpenSSL::SSL OpenSSL::Crypto
  nlohmann_json::nlohmann_json spdlog::spdlog tomlplusplus::tomlplusplus Threads::Threads
  utf8proc::utf8proc onnxruntime::onnxruntime)
semcache_set_warnings(semcache_core)

add_executable(semcache src/main.cpp)
target_link_libraries(semcache PRIVATE semcache_core)
semcache_set_warnings(semcache)

# Mock upstream: deterministic OpenAI-compatible server for tests, benchmarks and demos.
add_library(semcache_mock STATIC mock/mock_upstream.cpp)
target_include_directories(semcache_mock PUBLIC mock)
target_link_libraries(semcache_mock PUBLIC semcache_core)
semcache_set_warnings(semcache_mock)

add_executable(semcache-mock mock/main.cpp)
target_link_libraries(semcache-mock PRIVATE semcache_mock)
semcache_set_warnings(semcache-mock)

if(SEMCACHE_BUILD_BENCH)
  find_package(benchmark CONFIG REQUIRED)
  add_executable(semcache_bench_embed bench/embed_bench.cpp)
  target_link_libraries(semcache_bench_embed PRIVATE semcache_core benchmark::benchmark)
  target_compile_definitions(semcache_bench_embed PRIVATE SEMCACHE_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
  semcache_set_warnings(semcache_bench_embed)
  add_executable(semcache_bench_index bench/index_bench.cpp)
  target_link_libraries(semcache_bench_index PRIVATE semcache_core benchmark::benchmark)
  semcache_set_warnings(semcache_bench_index)
endif()

if(SEMCACHE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
