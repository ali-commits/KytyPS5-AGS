get_filename_component(instance_launcher "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_executable(launcher_gui_instance_tests
 "${instance_launcher}/tests/GuiInstanceTests.cpp"
 "${instance_launcher}/src/guiInstance.cpp" "${instance_launcher}/include/guiInstance.h"
 "${instance_launcher}/src/applicationLifetime.cpp" "${instance_launcher}/include/applicationLifetime.h")
set_target_properties(launcher_gui_instance_tests PROPERTIES AUTOMOC ON)
target_include_directories(launcher_gui_instance_tests PRIVATE "${instance_launcher}/include")
target_link_libraries(launcher_gui_instance_tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME launcher_gui_instance COMMAND launcher_gui_instance_tests)
set_tests_properties(launcher_gui_instance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
