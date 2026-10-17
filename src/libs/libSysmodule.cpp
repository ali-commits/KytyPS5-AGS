#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kernel/module.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <mutex>
#include <unordered_map>

namespace Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace LibKernel {
struct ModuleInfoForUnwind;
int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
} // namespace LibKernel

namespace Sysmodule {

constexpr int SYSMODULE_ERROR_UNLOADED = static_cast<int>(0x805a1001u);
constexpr int SYSMODULE_ERROR_FATAL    = static_cast<int>(0x805a10ffu);

static constexpr struct NativeModule {
	uint16_t    id;
	const char* path;
} g_native_modules[] = {{0x0115, "/app0/sce_module/libSceNpCppWebApi.prx"}};

enum class ModuleState { Loading, Loaded, Unloading };

struct Module {
	ModuleState             state      = ModuleState::Loading;
	uint32_t                references = 0;
	LibKernel::KernelModule handle     = -1;
};

// Lifecycle calls execute guest entry points, which may load dependencies. Serialize
// lifecycle operations across threads while allowing same-thread dependency loads;
// the explicit state rejects recursion into the module currently starting/stopping.
static std::recursive_mutex                 g_lifecycle_mutex;
static std::mutex                           g_module_mutex;
static std::unordered_map<uint16_t, Module> g_modules;

#if defined(KYTY_SYSMODULE_TESTS)
static LibKernel::KernelModule (*g_test_load)(const char*) = nullptr;
static int (*g_test_unload)(LibKernel::KernelModule)       = nullptr;

void TestSetNativeModuleCallbacks(LibKernel::KernelModule (*load)(const char*),
                                  int (*unload)(LibKernel::KernelModule)) {
	std::scoped_lock lifecycle(g_lifecycle_mutex);
	g_test_load   = load;
	g_test_unload = unload;
}
#endif

static LibKernel::KernelModule LoadNative(const char* path) {
#if defined(KYTY_SYSMODULE_TESTS)
	if (g_test_load != nullptr) return g_test_load(path);
#endif
	return LibKernel::KernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
}

static int UnloadNative(LibKernel::KernelModule handle) {
#if defined(KYTY_SYSMODULE_TESTS)
	if (g_test_unload != nullptr) return g_test_unload(handle);
#endif
	return LibKernel::KernelStopUnloadModule(handle, 0, nullptr, 0, nullptr, nullptr);
}

static int LoadModule(uint16_t id) {
	std::scoped_lock lifecycle(g_lifecycle_mutex);
	{
		std::scoped_lock lock(g_module_mutex);
		if (const auto module = g_modules.find(id); module != g_modules.end()) {
			if (module->second.state != ModuleState::Loaded) return SYSMODULE_ERROR_FATAL;
			++module->second.references;
			return 0;
		}
		g_modules.emplace(id, Module {});
	}

	LibKernel::KernelModule handle = -1;
	for (const auto& module: g_native_modules) {
		if (module.id != id) continue;
		handle = LoadNative(module.path);
		if (handle < 0) {
			std::scoped_lock lock(g_module_mutex);
			g_modules.erase(id);
			return handle;
		}
		break;
	}
	std::scoped_lock lock(g_module_mutex);
	g_modules.at(id) = Module {ModuleState::Loaded, 1, handle};
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags,
                                                         LibKernel::ModuleInfoForUnwind* info) {
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

static KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	return LoadModule(id);
}

static KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	std::scoped_lock        lifecycle(g_lifecycle_mutex);
	LibKernel::KernelModule handle = -1;
	{
		std::scoped_lock lock(g_module_mutex);
		const auto       module = g_modules.find(id);
		if (module == g_modules.end()) return SYSMODULE_ERROR_UNLOADED;
		if (module->second.state != ModuleState::Loaded) return SYSMODULE_ERROR_FATAL;
		if (module->second.references > 1) {
			--module->second.references;
			return 0;
		}
		module->second.state = ModuleState::Unloading;
		handle               = module->second.handle;
	}
	const bool       failed = handle >= 0 && UnloadNative(handle) < 0;
	std::scoped_lock lock(g_module_mutex);
	if (failed) {
		g_modules.at(id).state = ModuleState::Loaded;
		return SYSMODULE_ERROR_FATAL;
	}
	g_modules.erase(id);
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2,
                                                            int arg3, int* ret) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	EXIT_IF(arg1 != 0);
	EXIT_IF(arg2 != 0);
	EXIT_IF(arg3 != 0);
	EXIT_IF(ret == nullptr);

	*ret = LoadModule(id);
	return *ret;
}

static KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	std::scoped_lock lock(g_module_mutex);
	const auto       module = g_modules.find(id);
	return module != g_modules.end() && module->second.state == ModuleState::Loaded
	           ? 0
	           : SYSMODULE_ERROR_UNLOADED;
}

} // namespace Sysmodule

LIB_DEFINE(InitSysmodule_1) {
	LIB_FUNC("4fU5yvOkVG4", Sysmodule::SysmoduleGetModuleInfoForUnwind);
	LIB_FUNC("eR2bZFAAU0Q", Sysmodule::SysmoduleUnloadModule);
	LIB_FUNC("hHrGoGoNf+s", Sysmodule::SysmoduleLoadModuleInternalWithArg);
	LIB_FUNC("g8cM39EUZ6o", Sysmodule::SysmoduleLoadModule);
	LIB_FUNC("fMP5NHUOaMk", Sysmodule::SysmoduleIsLoaded);
}

} // namespace Libs
