# One CTest entry per suite. No test carries a timeout: a hang is a defect and
# is diagnosed rather than converted into a pass.
set(LQF_TEST_SUITES
  units_metric
  identity_capability
  counter_semantics
  counter_property
  gauge_freshness
  policy_generation
  classification_extremes
  conflicts
  window_history
  explain_derivation
  persistence
  persistence_kill
  concurrency
  shutdown
  wire_codec
  transport
  multiprocess
  lock_audit)

set(LQF_TEST_SOURCES main.cpp harness.cpp)
foreach(suite IN LISTS LQF_TEST_SUITES)
  list(APPEND LQF_TEST_SOURCES "test_${suite}.cpp")
endforeach()

add_executable(lqf_tests ${LQF_TEST_SOURCES})
target_link_libraries(lqf_tests PRIVATE lqf::lqf)
target_include_directories(lqf_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
lqf_apply_warnings(lqf_tests)

foreach(suite IN LISTS LQF_TEST_SUITES)
  add_test(NAME ${suite} COMMAND lqf_tests --suite ${suite})
endforeach()

set_tests_properties(${LQF_TEST_SUITES} PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
