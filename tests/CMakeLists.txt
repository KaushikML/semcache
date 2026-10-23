find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

set(SEMCACHE_TEST_ENV "")
if(SEMCACHE_SANITIZE MATCHES "address")
  set(SEMCACHE_TEST_ENV "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/lsan.supp")
elseif(SEMCACHE_SANITIZE MATCHES "thread")
  set(SEMCACHE_TEST_ENV
      "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1 suppressions=${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/tsan.supp")
endif()

add_executable(semcache_unit_tests
  unit/config_test.cpp
  unit/cache_store_test.cpp
  unit/cache_writer_test.cpp
  unit/coalescer_test.cpp
  unit/completion_codec_test.cpp
  unit/context_key_test.cpp
  unit/persistence_test.cpp
  unit/simd_test.cpp
  unit/vector_index_test.cpp
  unit/hnsw_test.cpp
  unit/log_test.cpp
  unit/metrics_test.cpp
  unit/rate_limiter_test.cpp
  unit/tenants_test.cpp
  unit/mock_response_test.cpp
  unit/bert_normalizer_test.cpp
  unit/tokenizer_test.cpp
  unit/worker_pool_test.cpp
  unit/embedder_test.cpp
)
target_include_directories(semcache_unit_tests PRIVATE .)
target_compile_definitions(semcache_unit_tests PRIVATE
  SEMCACHE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  SEMCACHE_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
target_link_libraries(semcache_unit_tests PRIVATE semcache_core semcache_mock GTest::gtest_main)
gtest_discover_tests(semcache_unit_tests DISCOVERY_TIMEOUT 30 PROPERTIES ENVIRONMENT "${SEMCACHE_TEST_ENV}")

add_executable(semcache_integration_tests
  integration/passthrough_test.cpp
  integration/streaming_test.cpp
  integration/failure_test.cpp
  integration/cache_test.cpp
  integration/herd_test.cpp
  integration/restart_purge_test.cpp
  integration/semantic_model_test.cpp
)
target_include_directories(semcache_integration_tests PRIVATE .)
target_compile_definitions(semcache_integration_tests PRIVATE
  SEMCACHE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  SEMCACHE_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
target_link_libraries(semcache_integration_tests PRIVATE semcache_core semcache_mock GTest::gtest_main)
gtest_discover_tests(semcache_integration_tests DISCOVERY_TIMEOUT 30 PROPERTIES ENVIRONMENT "${SEMCACHE_TEST_ENV}")
