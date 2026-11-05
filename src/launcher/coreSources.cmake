# Sources of kyty_launcher_core: Qt Core/Gui/Network code with no widgets. Included by the
# launcher, the tests and (later) the AGS adapter.
set(_core_dir "${CMAKE_CURRENT_LIST_DIR}")
set(launcher_core_sources)
foreach(_name gameLibrary gameContent configuration emulatorCommand ownerPreferences cheatFile
		cheatRepository compatibilityDatabase gameServices)
	list(APPEND launcher_core_sources "${_core_dir}/src/${_name}.cpp" "${_core_dir}/include/${_name}.h")
endforeach()
list(APPEND launcher_core_sources "${_core_dir}/include/preferenceSchema.h"
	"${_core_dir}/include/preferenceDevices.h")
