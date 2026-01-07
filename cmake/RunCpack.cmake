if(DEFINED ENV{CPACK_RUNNING})
  message(STATUS "Skipping cpack during cpack preinstall")
  return()
endif()

if(NOT DEFINED CPACK_CMD)
  message(FATAL_ERROR "CPACK_CMD is required")
endif()

if(NOT DEFINED CPACK_OUT_DIR)
  set(CPACK_OUT_DIR ".")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env CPACK_RUNNING=1
          "${CPACK_CMD}" -G DEB -B "${CPACK_OUT_DIR}"
  RESULT_VARIABLE cpack_result
)

if(NOT cpack_result EQUAL 0)
  message(FATAL_ERROR "CPack failed with code ${cpack_result}")
endif()
