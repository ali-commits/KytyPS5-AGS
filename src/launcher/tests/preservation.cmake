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
    target_link_libraries(launcher_preservation_tests PRIVATE common kyty_git_version Vulkan::Headers
        LibAtrac9 FFmpeg::ffmpeg)
else()
    # Optional standalone use reuses existing dependency bytes; it never configures/builds them.
    if(NOT KYTY_LAUNCHER_WARM_BUILD)
        message(FATAL_ERROR "Set KYTY_LAUNCHER_WARM_BUILD to an existing native build for preservation tests")
    endif()
    target_include_directories(launcher_preservation_tests PRIVATE "${KYTY_LAUNCHER_WARM_BUILD}"
        "${KYTY_LAUNCHER_WARM_BUILD}/../../3rdparty/Vulkan-Headers/include"
        "${KYTY_LAUNCHER_WARM_BUILD}/../../3rdparty/nlohmann_json/include")
    if(EXISTS "${preservation_root}/src/common/additionalContent.cpp")
        target_sources(launcher_preservation_tests PRIVATE "${preservation_root}/src/common/additionalContent.cpp")
    endif()
    target_link_libraries(launcher_preservation_tests PRIVATE
        "${KYTY_LAUNCHER_WARM_BUILD}/common/libcommon.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/tracy/libTracyClient.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/cpuinfo/libcpuinfo.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/spdlog/libspdlog.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/fmt/libfmt.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/libzarchive.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/_deps/zstd-build/lib/libzstd.a" ${CMAKE_DL_LIBS})
    # Reuse the exact FFmpeg package already used by the warm native build.
    file(GLOB preservation_ffmpeg_headers
        "${KYTY_LAUNCHER_WARM_BUILD}/externals/ffmpeg-*/include/libavcodec/avcodec.h")
    list(LENGTH preservation_ffmpeg_headers preservation_ffmpeg_count)
    if(NOT preservation_ffmpeg_count EQUAL 1)
        message(FATAL_ERROR "The warm build must contain exactly one unpacked FFmpeg package")
    endif()
    list(GET preservation_ffmpeg_headers 0 preservation_ffmpeg_header)
    get_filename_component(preservation_ffmpeg "${preservation_ffmpeg_header}/../../.." ABSOLUTE)
    target_include_directories(launcher_preservation_tests PRIVATE
        "${preservation_ffmpeg}/include" "${preservation_root}/3rdparty/LibAtrac9/C/src")
    target_link_libraries(launcher_preservation_tests PRIVATE
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/libLibAtrac9.a")
    foreach(preservation_codec avformat avcodec swscale avutil avfilter swresample)
        target_link_libraries(launcher_preservation_tests PRIVATE
            "${preservation_ffmpeg}/lib/lib${preservation_codec}.a")
    endforeach()
    find_package(Threads REQUIRED)
    target_link_libraries(launcher_preservation_tests PRIVATE m Threads::Threads)
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
