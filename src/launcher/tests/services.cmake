# Small tests for the reusable (non-widget) launcher code. They compile only the sources they use.
get_filename_component(services_launcher "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

add_executable(launcher_owner_preferences_tests
    "${services_launcher}/tests/ownerPreferencesTests.cpp"
    "${services_launcher}/src/ownerPreferences.cpp" "${services_launcher}/src/configuration.cpp"
    "${services_launcher}/include/configuration.h")
set_target_properties(launcher_owner_preferences_tests PROPERTIES AUTOMOC ON)
target_include_directories(launcher_owner_preferences_tests PRIVATE "${services_launcher}/include")
target_link_libraries(launcher_owner_preferences_tests PRIVATE Qt6::Core Qt6::Gui
    SDL3::SDL3-static Vulkan::Headers common)
add_test(NAME launcher_owner_preferences COMMAND launcher_owner_preferences_tests)
set_tests_properties(launcher_owner_preferences PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIO_DRIVER=dummy")

add_executable(launcher_game_services_tests
    "${services_launcher}/tests/gameServicesTests.cpp"
    "${services_launcher}/src/gameServices.cpp" "${services_launcher}/src/gameContent.cpp"
    "${services_launcher}/src/cheatFile.cpp" "${services_launcher}/src/cheatRepository.cpp"
    "${services_launcher}/src/configuration.cpp" "${services_launcher}/include/configuration.h"
    "${services_launcher}/include/cheatRepository.h")
set_target_properties(launcher_game_services_tests PROPERTIES AUTOMOC ON)
target_include_directories(launcher_game_services_tests PRIVATE "${services_launcher}/include")
target_link_libraries(launcher_game_services_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network common)
add_test(NAME launcher_game_services COMMAND launcher_game_services_tests)
set_tests_properties(launcher_game_services PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
