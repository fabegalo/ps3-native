#include "launcher_config.h"
#include <cstdio>
#include <vector>
#include <string>
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
    // _wspawnv concatenates arguments; preserve Windows CRT quoting explicitly.
    auto quote = [](const wchar_t* value) {
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (; *value; ++value) {
            if (*value == L'\\') { ++slashes; continue; }
            out.append(slashes * (*value == L'"' ? 2 : 1), L'\\');
            if (*value == L'"') out += L'\\';
            out += *value;
            slashes = 0;
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
        return out;
    };
    std::vector<std::wstring> quoted{quote(python.data()), quote(script.data())};
    for (int i = 1; i < argc; ++i) quoted.push_back(quote(argv[i]));
    std::vector<const wchar_t*> args;
    for (const auto& value : quoted) args.push_back(value.c_str());
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
