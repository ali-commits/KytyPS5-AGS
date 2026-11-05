# kyty-ags-adapter: the AGS extension adapter (protocol 1). QtCore event loop, no widgets.
# Built next to kyty_emulator and installed at the package root, so $ORIGIN/lib finds Qt.
find_package(Qt6 6.8 COMPONENTS Core Gui Network REQUIRED)

set(ags_adapter_lib_sources
	framing.cpp framing.h
	reply.h
	adapterState.cpp adapterState.h
	settings.cpp settings.h
	library.cpp library.h
	actions.cpp actions.h
	launchPlan.cpp launchPlan.h
	adapter.cpp adapter.h)

add_executable(kyty-ags-adapter main.cpp ${ags_adapter_lib_sources})
set_target_properties(kyty-ags-adapter PROPERTIES
	AUTOMOC ON
	RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:kyty_emulator>")
target_link_libraries(kyty-ags-adapter PRIVATE kyty_launcher_core)
# hello.version, and (P2-6) the manifest version, come from the CMake project version.
target_compile_definitions(kyty-ags-adapter PRIVATE KYTY_AGS_ADAPTER_VERSION="${PROJECT_VERSION}")
if (CLANG AND NOT KYTY_CLANG_CL)
	target_compile_options(kyty-ags-adapter PRIVATE -frtti)
endif()
if(LINUX)
	set_target_properties(kyty-ags-adapter PROPERTIES
		INSTALL_RPATH "$ORIGIN/lib"
		BUILD_WITH_INSTALL_RPATH FALSE
		LINK_FLAGS "${KYTY_LD_OPTIONS} -Wl,--disable-new-dtags")
	target_link_options(kyty-ags-adapter PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
endif()
install(TARGETS kyty-ags-adapter RUNTIME DESTINATION .)
# The adapter puts the preset into each launch.plan env (next to the adapter, as in the release).
install(FILES "${CMAKE_SOURCE_DIR}/tools/u59-preset.json" DESTINATION .)

if(BUILD_TESTING)
	find_package(Qt6 REQUIRED COMPONENTS Test)
	add_executable(kyty_ags_adapter_tests tests/adapterTests.cpp framing.cpp framing.h)
	target_include_directories(kyty_ags_adapter_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
	target_link_libraries(kyty_ags_adapter_tests PRIVATE Qt6::Core Qt6::Gui)
	target_compile_definitions(kyty_ags_adapter_tests PRIVATE
		KYTY_AGS_ADAPTER_VERSION="${PROJECT_VERSION}"
		KYTY_TEST_ADAPTER="$<TARGET_FILE:kyty-ags-adapter>"
		KYTY_TEST_PRESET="${CMAKE_SOURCE_DIR}/tools/u59-preset.json")
	add_dependencies(kyty_ags_adapter_tests kyty-ags-adapter)
	add_test(NAME kyty_ags_adapter COMMAND kyty_ags_adapter_tests)
	set_tests_properties(kyty_ags_adapter PROPERTIES TIMEOUT 120
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
