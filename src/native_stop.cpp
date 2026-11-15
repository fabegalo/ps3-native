#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" [[noreturn]] void ps3_native_stop(void) {
    // Fatal diagnostics cannot depend on teardown locks held by guest threads.
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 86);
#endif
    std::_Exit(86);
}
