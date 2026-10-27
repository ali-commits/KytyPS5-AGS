# Compile the real launcher consumers, so catalog inheritance and the dialog cannot drift
# from the shared argument builder while standalone Qt checks stay independent of the core.
get_filename_component(host_launcher "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(host_root "${host_launcher}/../.." ABSOLUTE)
find_package(Qt6 REQUIRED COMPONENTS Concurrent Network Widgets Test)
file(GLOB host_sources "${host_launcher}/src/*.cpp" "${host_launcher}/include/*.h")
list(REMOVE_ITEM host_sources "${host_launcher}/src/main.cpp")
add_executable(launcher_host_tests
    "${host_launcher}/tests/LauncherHostTests.cpp"
    ${host_sources} "${host_launcher}/resources.qrc")
set_target_properties(launcher_host_tests PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    AUTOUIC_SEARCH_PATHS "${host_launcher}/forms")
target_include_directories(launcher_host_tests PRIVATE
    "${host_launcher}/include" "${host_launcher}/src" "${host_root}/src")
target_link_libraries(launcher_host_tests PRIVATE Qt6::Concurrent Qt6::Network Qt6::Widgets Qt6::Test SDL3::SDL3-static)
if(TARGET common)
    target_link_libraries(launcher_host_tests PRIVATE common kyty_git_version Vulkan::Headers
        LibAtrac9 FFmpeg::ffmpeg)
else()
    # Optional standalone use reuses existing dependency bytes; it never configures/builds them.
    if(NOT KYTY_LAUNCHER_WARM_BUILD)
        message(FATAL_ERROR "Set KYTY_LAUNCHER_WARM_BUILD to an existing native build for host tests")
    endif()
    target_include_directories(launcher_host_tests PRIVATE "${KYTY_LAUNCHER_WARM_BUILD}"
        "${KYTY_LAUNCHER_WARM_BUILD}/../../3rdparty/Vulkan-Headers/include"
        "${KYTY_LAUNCHER_WARM_BUILD}/../../3rdparty/nlohmann_json/include")
    if(EXISTS "${host_root}/src/common/additionalContent.cpp")
        target_sources(launcher_host_tests PRIVATE "${host_root}/src/common/additionalContent.cpp")
    endif()
    target_link_libraries(launcher_host_tests PRIVATE
        "${KYTY_LAUNCHER_WARM_BUILD}/common/libcommon.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/tracy/libTracyClient.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/cpuinfo/libcpuinfo.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/spdlog/libspdlog.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/fmt/libfmt.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/libzarchive.a"
        "${KYTY_LAUNCHER_WARM_BUILD}/_deps/zstd-build/lib/libzstd.a" ${CMAKE_DL_LIBS})
    # Reuse the exact FFmpeg package already used by the warm native build.
    file(GLOB host_ffmpeg_headers
        "${KYTY_LAUNCHER_WARM_BUILD}/externals/ffmpeg-*/include/libavcodec/avcodec.h")
    list(LENGTH host_ffmpeg_headers host_ffmpeg_count)
    if(NOT host_ffmpeg_count EQUAL 1)
        message(FATAL_ERROR "The warm build must contain exactly one unpacked FFmpeg package")
    endif()
    list(GET host_ffmpeg_headers 0 host_ffmpeg_header)
    get_filename_component(host_ffmpeg "${host_ffmpeg_header}/../../.." ABSOLUTE)
    target_include_directories(launcher_host_tests PRIVATE
        "${host_ffmpeg}/include" "${host_root}/3rdparty/LibAtrac9/C/src")
    target_link_libraries(launcher_host_tests PRIVATE
        "${KYTY_LAUNCHER_WARM_BUILD}/3rdparty/libLibAtrac9.a")
    foreach(host_codec avformat avcodec swscale avutil avfilter swresample)
        target_link_libraries(launcher_host_tests PRIVATE
            "${host_ffmpeg}/lib/lib${host_codec}.a")
    endforeach()
    find_package(Threads REQUIRED)
    target_link_libraries(launcher_host_tests PRIVATE m Threads::Threads)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(launcher_host_tests PRIVATE Qt6::DBus)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(launcher_host_tests PRIVATE -frtti)
endif()
add_test(NAME launcher_host COMMAND launcher_host_tests)
set_tests_properties(launcher_host PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIO_DRIVER=dummy")
