# Small tests for kyty_launcher_core (the reusable, non-widget launcher code).
get_filename_component(services_launcher "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

add_executable(launcher_owner_preferences_tests "${services_launcher}/tests/ownerPreferencesTests.cpp")
target_link_libraries(launcher_owner_preferences_tests PRIVATE kyty_launcher_core)
add_test(NAME launcher_owner_preferences COMMAND launcher_owner_preferences_tests)
set_tests_properties(launcher_owner_preferences PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIO_DRIVER=dummy")

add_executable(launcher_game_services_tests "${services_launcher}/tests/gameServicesTests.cpp")
target_link_libraries(launcher_game_services_tests PRIVATE kyty_launcher_core)
add_test(NAME launcher_game_services COMMAND launcher_game_services_tests)
set_tests_properties(launcher_game_services PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(launcher_core_tests "${services_launcher}/tests/coreTests.cpp")
target_link_libraries(launcher_core_tests PRIVATE kyty_launcher_core)
add_test(NAME launcher_core COMMAND launcher_core_tests)
set_tests_properties(launcher_core PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
