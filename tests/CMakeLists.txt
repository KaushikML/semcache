find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(semcache_unit_tests
  unit/config_test.cpp
  unit/log_test.cpp
  unit/metrics_test.cpp
  unit/rate_limiter_test.cpp
  unit/tenants_test.cpp
  unit/mock_response_test.cpp
)
target_include_directories(semcache_unit_tests PRIVATE .)
target_link_libraries(semcache_unit_tests PRIVATE semcache_core semcache_mock GTest::gtest_main)
gtest_discover_tests(semcache_unit_tests DISCOVERY_TIMEOUT 30)

add_executable(semcache_integration_tests
  integration/passthrough_test.cpp
  integration/streaming_test.cpp
  integration/failure_test.cpp
)
target_include_directories(semcache_integration_tests PRIVATE .)
target_link_libraries(semcache_integration_tests PRIVATE semcache_core semcache_mock GTest::gtest_main)
gtest_discover_tests(semcache_integration_tests DISCOVERY_TIMEOUT 30)
