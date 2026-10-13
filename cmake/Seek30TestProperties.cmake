# Apply after GoogleTest discovery. DISCOVERY_TIMEOUT only bounds discovery;
# this timeout also bounds a stalled worker or a blocking future destructor.
list(
  FIND
  testsuite
  "Seek30WorkerStateTest.OwningWorkerRefreshesAfterProducerCueEdit"
  _seek30_worker_test_index
)
if(NOT _seek30_worker_test_index EQUAL -1)
  set_tests_properties(
    Seek30WorkerStateTest.OwningWorkerRefreshesAfterProducerCueEdit
    PROPERTIES TIMEOUT 15
  )
endif()
unset(_seek30_worker_test_index)
