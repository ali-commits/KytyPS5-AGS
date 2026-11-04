cmake_minimum_required(VERSION 3.24)
project(KytyLauncherStreamingTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Network Widgets Test)
add_executable(launcher_streaming_tests ../../../tests/LauncherStreamingTests.cpp ../src/streamingSettings.cpp ../src/streamingCatalog.cpp ../src/streamingAudio.cpp ../src/streamingApi.cpp ../src/streamingBackend.cpp ../src/streamingWidget.cpp ../src/applicationLifetime.cpp ../include/applicationLifetime.h ../src/hostControlClient.cpp ../src/hostSocket.cpp ../include/hostControlClient.h ../include/hostSocket.h ../include/streamingAudio.h ../include/streamingApi.h ../include/streamingBackend.h ../include/streamingWidget.h)
target_include_directories(launcher_streaming_tests PRIVATE ../include)
target_link_libraries(launcher_streaming_tests PRIVATE Qt6::Core Qt6::Concurrent Qt6::Network Qt6::Widgets Qt6::Test)
enable_testing()
add_test(NAME launcher_streaming COMMAND launcher_streaming_tests)
set_tests_properties(launcher_streaming PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(kyty_stream_launch ../tools/streamLaunch.cpp)
target_link_libraries(kyty_stream_launch PRIVATE Qt6::Core)
set_target_properties(kyty_stream_launch PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libexec")

target_compile_definitions(launcher_streaming_tests PRIVATE KYTY_LAUNCHER_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/LauncherStreamingFixtures.py")

add_dependencies(launcher_streaming_tests kyty_stream_launch)

if(KYTY_LAUNCHER_WARM_BUILD)
    find_package(SDL3 CONFIG REQUIRED)
    include(preservation.cmake)
endif()

include(lifetime.cmake)
include(gui-instance.cmake)

add_executable(launcher_library_page_tests libraryPageTests.cpp ../src/libraryPage.cpp ../include/libraryPage.h ../resources.qrc)
set_target_properties(launcher_library_page_tests PROPERTIES AUTORCC ON)
target_include_directories(launcher_library_page_tests PRIVATE ../include)
target_link_libraries(launcher_library_page_tests PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME launcher_library_page COMMAND launcher_library_page_tests)
set_tests_properties(launcher_library_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

include(artwork.cmake)

include(remote.cmake)

if(KYTY_LAUNCHER_WARM_BUILD)
    add_executable(launcher_owner_preferences_tests ownerPreferencesTests.cpp ../src/ownerPreferences.cpp ../src/configuration.cpp ../include/configuration.h)
    target_include_directories(launcher_owner_preferences_tests PRIVATE ../include ../../ "${KYTY_LAUNCHER_WARM_BUILD}")
    target_link_libraries(launcher_owner_preferences_tests PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME launcher_owner_preferences COMMAND launcher_owner_preferences_tests)
    include(host.cmake)
endif()
