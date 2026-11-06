# ctest kyty_ags_adapter_install: install the build into a temp prefix, then run the adapter tests
# with KYTY_TEST_INSTALL_ROOT set so every published icon must exist in the installed tree.
# The install needs the full build (launcher, emulator); the release workflow has it.
if(NOT EXISTS "${BUILD_DIR}/launcher/kyty_launcher")
	message("SKIP: launcher not built, no installable tree")
	return()
endif()
set(prefix "${BUILD_DIR}/ags-install-test")
file(REMOVE_RECURSE "${prefix}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}"
	RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "cmake --install failed: ${rc}")
endif()
set(ENV{KYTY_TEST_INSTALL_ROOT} "${prefix}")
set(ENV{QT_QPA_PLATFORM} offscreen)
execute_process(COMMAND "${TESTER}" RESULT_VARIABLE rc)
file(REMOVE_RECURSE "${prefix}")
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "installed-tree icon check failed: ${rc}")
endif()
