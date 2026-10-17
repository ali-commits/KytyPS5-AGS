# Compile the real launcher consumers, so catalog inheritance and the dialog cannot drift
# from the shared argument builder while standalone Qt checks stay independent of the core.
get_filename_component(preservation_launcher "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(preservation_root "${preservation_launcher}/../.." ABSOLUTE)
find_package(Qt6 REQUIRED COMPONENTS Concurrent Network Widgets Test)
file(GLOB preservation_sources "${preservation_launcher}/src/*.cpp" "${preservation_launcher}/include/*.h")
list(REMOVE_ITEM preservation_sources "${preservation_launcher}/src/main.cpp")
add_executable(launcher_preservation_tests
    "${preservation_root}/tests/LauncherPreservationTests.cpp"
    ${preservation_sources} "${preservation_launcher}/resources.qrc")
set_target_properties(launcher_preservation_tests PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    AUTOUIC_SEARCH_PATHS "${preservation_launcher}/forms")
target_include_directories(launcher_preservation_tests PRIVATE
    "${preservation_launcher}/include" "${preservation_launcher}/src" "${preservation_root}/src")
target_link_libraries(launcher_preservation_tests PRIVATE Qt6::Concurrent Qt6::Network Qt6::Widgets Qt6::Test SDL3::SDL3-static)
if(TARGET common)
    target_link_libraries(launcher_preservation_tests PRIVATE common kyty_git_version Vulkan::Headers)
else()
    # Optional standalone use reuses existing dependency bytes; it never configures/builds them.
    if(NOT KYTY_LAUNCHER_WARM_BUILD)
        message(FATAL_ERROR "Set KYTY_LAUNCHER_WARM_BUILD to an existing native build for preservation tests")
    endif()
    target_include_directories(launcher_preservation_tests PRIVATE "${KYTY_LAUNCHER_WARM_BUILD}"
        "${KYTY_LAUNCHER_WARM_BUILD}/../../3rdparty/Vulkan-Headers/include")
    target_link_libraries(launcher_preservation_tests PRIVATE
        "${KYTY_LAUNCHER_WARM_BUILD}/common/libcommon.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/tracy/libTracyClient.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/cpuinfo/libcpuinfo.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/spdlog/libspdlog.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/fmt/libfmt.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/libzarchive.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/_deps/zstd-build/lib/libzstd.a" ${CMAKE_DL_LIBS})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(launcher_preservation_tests PRIVATE Qt6::DBus)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(launcher_preservation_tests PRIVATE -frtti)
endif()
add_test(NAME launcher_preservation COMMAND launcher_preservation_tests)
set_tests_properties(launcher_preservation PROPERTIES TIMEOUT 45
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIO_DRIVER=dummy")
