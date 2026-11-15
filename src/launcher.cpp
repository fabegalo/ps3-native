#include "launcher_config.h"
#include <cstdio>
#include <vector>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
int wmain(int argc, wchar_t** argv) {
    auto wide = [](const char* s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::vector<wchar_t> v(n);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, v.data(), n);
        return v;
    };
    auto python = wide(PS3_NATIVE_PYTHON), script = wide(PS3_NATIVE_CLI);
    std::vector<const wchar_t*> args{python.data(), script.data()};
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    auto rc = _wspawnv(_P_WAIT, python.data(), args.data());
    if (rc == -1) { perror("Python launcher"); return 1; }
    return static_cast<int>(rc);
}
#else
#include <unistd.h>
int main(int argc, char** argv) {
    std::vector<const char*> args{PS3_NATIVE_PYTHON, PS3_NATIVE_CLI};
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    execv(PS3_NATIVE_PYTHON, const_cast<char* const*>(args.data()));
    perror("Python launcher");
    return 1;
}
#endif
