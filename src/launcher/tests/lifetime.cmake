get_filename_component(lifetime_launcher "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_executable(launcher_lifetime_tests
 "${lifetime_launcher}/tests/LauncherLifetimeTests.cpp"
 "${lifetime_launcher}/resources.qrc"
 "${lifetime_launcher}/src/applicationLifetime.cpp" "${lifetime_launcher}/include/applicationLifetime.h"
 "${lifetime_launcher}/src/hostControlClient.cpp" "${lifetime_launcher}/include/hostControlClient.h"
 "${lifetime_launcher}/src/hostSocket.cpp" "${lifetime_launcher}/include/hostSocket.h")
set_target_properties(launcher_lifetime_tests PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(launcher_lifetime_tests PRIVATE "${lifetime_launcher}/include")
target_link_libraries(launcher_lifetime_tests PRIVATE Qt6::Widgets Qt6::Test Qt6::Concurrent Qt6::Network)
add_test(NAME launcher_lifetime COMMAND launcher_lifetime_tests)
set_tests_properties(launcher_lifetime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
