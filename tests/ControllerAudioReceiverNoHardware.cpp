// Preloaded only by the receiver loopback tests. Any attempt to initialize SDL
// or open a physical audio/HID node makes the subprocess fail immediately.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
namespace {
void Reject(const char* action) {
	std::fprintf(stderr, "null receiver attempted physical output: %s\n", action);
	std::abort();
}
} // namespace
extern "C" bool SDL_Init(unsigned int) {
	Reject("SDL_Init");
	return false;
}
extern "C" bool SDL_InitSubSystem(unsigned int) {
	Reject("SDL_InitSubSystem");
	return false;
}
extern "C" int open(const char* path, int flags, ...) {
	if (std::strncmp(path, "/dev/hidraw", 11) == 0 || std::strncmp(path, "/dev/snd/", 9) == 0)
		Reject(path);
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	using Fn         = int (*)(const char*, int, ...);
	static auto real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "open"));
	return real(path, flags, mode);
}
