# cmake/run_test_app.cmake
# Called by CTest to run test_app under LD_PRELOAD and capture stderr to a log file.
# Variables injected by CMakeLists.txt:
#   TEST_APP      — path to test_app executable
#   MEMTRACK_SO   — path to memtrack.so
#   LOG_FILE      — path to write the memtrack log (stderr of test_app)

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
                "LD_PRELOAD=${MEMTRACK_SO}"
                "MEMTRACK_STACK_DEPTH=64"
            ${TEST_APP}
    RESULT_VARIABLE exit_code
    ERROR_FILE      ${LOG_FILE}
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "test_app exited with code ${exit_code}")
endif()
