#include "native_storage.h"
#include <filesystem>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <cerrno>
#include <sys/stat.h>
#include "ps3emu/error_codes.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#endif

extern "C" [[noreturn]] void ps3_native_stop(void);
namespace {
namespace fs = std::filesystem;
constexpr int32_t param = (int32_t)0x8002CB07u, failure = (int32_t)0x8002CB22u;
std::mutex install_lock;
using Fields = std::map<std::string, std::pair<std::string, uint32_t>>;
struct Installation { fs::path root, transaction, data, target; std::string name, guest; Fields fields; } pending;
[[noreturn]] void stop(const char* reason) {
    std::fprintf(stderr, "[native-blocked] installation: %s\n", reason);
    std::fflush(stderr); ps3_native_stop();
}
bool component(const std::string& name) {
    return !name.empty() && name.size() < 32 && name != "." && name != ".." && name.find_first_of("/\\:") == name.npos;
}
fs::path root_path() {
    fs::path root;
#ifdef _WIN32
    if (const auto* p = _wgetenv(L"PS3_HDD0_ROOT")) root = p;
#else
    if (const auto* p = std::getenv("PS3_HDD0_ROOT")) root = fs::u8path(p);
#endif
    if (root.empty()) stop("explicit HDD mount missing");
    std::error_code ec;
    root = fs::canonical(root, ec);
    if (ec || !fs::is_directory(root, ec) || ec) stop("HDD mount is unavailable");
    return root;
}
bool contained(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    auto actual = fs::weakly_canonical(path, ec);
    if (ec) stop("cannot resolve HDD path");
    auto i = actual.begin();
    for (const auto& part : root) if (i == actual.end() || *i++ != part) return false;
    return true;
}
FILE* open_file(const fs::path& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(path.c_str(), !std::strcmp(mode, "rb") ? L"rb" : L"wbx");
#else
    return std::fopen(path.c_str(), mode);
#endif
}
std::vector<uint8_t> read_file(const fs::path& path) {
    FILE* f = open_file(path, "rb");
    if (!f) stop("cannot read recovery metadata");
    std::vector<uint8_t> bytes(4096);
    auto n = std::fread(bytes.data(), 1, bytes.size(), f);
    const bool failed = std::ferror(f) || !std::feof(f);
    std::fclose(f);
    if (failed) stop("invalid or oversized recovery metadata");
    bytes.resize(n); return bytes;
}
void write_new(const fs::path& path, const std::vector<uint8_t>& bytes) {
    FILE* f = open_file(path, "wbx");
    if (!f) stop("cannot create exclusive metadata file");
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && !std::fflush(f);
#ifdef _WIN32
    ok = ok && !_commit(_fileno(f));
#else
    ok = ok && !fsync(fileno(f));
#endif
    ok = !std::fclose(f) && ok;
    if (!ok) stop("metadata write or synchronization failed");
}
void le16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void le32(std::vector<uint8_t>& b, uint32_t v) { le16(b, (uint16_t)v); le16(b, (uint16_t)(v >> 16)); }
std::vector<uint8_t> encode(const Fields& fields) {
    std::vector<uint8_t> keys, values, entries;
    for (const auto& [key, field] : fields) {
        while (values.size() % 4) values.push_back(0);
        const auto length = (uint32_t)field.first.size() + 1;
        le16(entries, (uint16_t)keys.size()); le16(entries, 0x204);
        le32(entries, length); le32(entries, field.second); le32(entries, (uint32_t)values.size());
        keys.insert(keys.end(), key.begin(), key.end()); keys.push_back(0);
        values.insert(values.end(), field.first.begin(), field.first.end()); values.push_back(0);
        values.resize(values.size() + field.second - length);
    }
    while (keys.size() % 4) keys.push_back(0);
    std::vector<uint8_t> result{0,'P','S','F'};
    le32(result, 0x101); le32(result, 20 + (uint32_t)entries.size());
    le32(result, 20 + (uint32_t)entries.size() + (uint32_t)keys.size()); le32(result, (uint32_t)fields.size());
    result.insert(result.end(), entries.begin(), entries.end()); result.insert(result.end(), keys.begin(), keys.end());
    result.insert(result.end(), values.begin(), values.end()); return result;
}
void copy_paths(const std::string& guest, char* info, char* usrdir) {
    if (guest.size() + 8 >= 128) stop("guest installation path exceeds ABI buffer");
    if (info) std::strcpy(info, guest.c_str());
    if (usrdir) std::strcpy(usrdir, (guest + "/USRDIR").c_str());
}
}
extern "C" int ps3_native_install_validate(const char* name) {
    if (!name || !component(name)) return 0;
    auto root = root_path();
    try { return contained(root, root / "game" / fs::u8path(name) / "PARAM.SFO"); }
    catch (const fs::filesystem_error&) { return 0; }
}
extern "C" int32_t ps3_native_install_create(const char* name, const uint8_t* init, char* info, char* usrdir) try {
    std::lock_guard lock(install_lock);
    if (!name || !component(name) || !init) return param;
    if (!pending.name.empty()) return failure;
    const char* identity = std::getenv("PS3_NATIVE_CONTENT_ID");
    if (!identity || std::strlen(identity) != 64 || std::strspn(identity, "0123456789abcdef") != 64)
        stop("verified content identity missing");
    Installation next;
    next.fields["CATEGORY"] = {"GD", 3};
    struct Input { const char* key; unsigned offset, capacity; };
    for (const auto& input : {Input{"TITLE",0,128}, Input{"TITLE_ID",128,10}, Input{"VERSION",140,6}}) {
        const auto* end = (const uint8_t*)std::memchr(init + input.offset, 0, input.capacity);
        if (!end) return param;
        next.fields[input.key] = {std::string((const char*)init + input.offset, (size_t)(end - init - input.offset)), input.capacity};
    }
    next.root = root_path(); next.name = name;
    const std::string suffix = next.name + '-' + std::string(identity, 16);
    next.transaction = next.root / ".native-install" / suffix;
    next.data = next.transaction / "data";
    next.target = next.root / "game" / fs::u8path(name);
    next.guest = "/dev_hdd0/.native-install/" + suffix + "/data";
    if (!contained(next.root, next.transaction / "owner") || !contained(next.root, next.target)) stop("escaping installation path");
    if (fs::exists(next.target)) stop("existing target requires explicit recovery; preserved unchanged");
    std::string owner = "ps3-native-install-v1\n" + std::string(identity) + '\n';
    auto initial = encode(next.fields);
    std::vector<uint8_t> marker(owner.begin(), owner.end()); marker.insert(marker.end(), initial.begin(), initial.end());
    std::error_code ec;
    fs::create_directories(next.transaction.parent_path(), ec); if (ec) stop("cannot create transaction parent");
    if (fs::create_directory(next.transaction, ec)) write_new(next.transaction / "owner", marker);
    else if (ec || read_file(next.transaction / "owner") != marker) stop("recovery identity or metadata mismatch");
    if (!contained(next.root, next.data / "USRDIR")) stop("escaping recovered installation");
    fs::create_directories(next.data / "USRDIR", ec); if (ec) stop("cannot create installation directory");
    copy_paths(next.guest, info, usrdir);
    pending = std::move(next);
    return 0;
} catch (const fs::filesystem_error&) { stop("host error while staging installation");
}
extern "C" int32_t ps3_native_install_permit(const char* name, char* info, char* usrdir) try {
    std::lock_guard lock(install_lock);
    if (pending.name.empty() || !name || pending.name != name) return failure;
    if (!contained(pending.root, pending.data / "PARAM.SFO") || !contained(pending.root, pending.target)) stop("installation path changed");
    uint64_t bytes = 0; unsigned entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(pending.data)) {
        // shortcut: bounded installation validation; increase only with measured title requirements.
        if (++entries > 200 || fs::is_symlink(entry.symlink_status())) stop("installation entry budget or unsupported link");
        if (!contained(pending.data, entry.path())) stop("escaping staged file");
        if (entry.is_regular_file()) bytes += entry.file_size();
        else if (!entry.is_directory()) stop("unsupported staged file type");
        if (bytes > 1024ull * 1024 * 1024) stop("installation exceeds 1 GiB validation budget");
    }
    auto sfo = pending.data / "PARAM.SFO";
    auto encoded = encode(pending.fields);
    if (fs::exists(sfo)) { if (read_file(sfo) != encoded) stop("staged PARAM.SFO differs; preserved unchanged"); }
    else write_new(sfo, encoded);
    std::error_code ec;
    fs::create_directories(pending.target.parent_path(), ec); if (ec) stop("cannot create game-data parent");
#ifdef _WIN32
    const bool moved = MoveFileExW(pending.data.c_str(), pending.target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__linux__)
    const bool moved = syscall(SYS_renameat2, AT_FDCWD, pending.data.c_str(), AT_FDCWD, pending.target.c_str(), RENAME_NOREPLACE) == 0;
#elif defined(__APPLE__)
    const bool moved = renamex_np(pending.data.c_str(), pending.target.c_str(), RENAME_EXCL) == 0;
#else
    const bool moved = false;
#endif
    if (!moved) stop("atomic publication failed; original target and recovery data preserved");
#ifndef _WIN32
    int parent = open(pending.target.parent_path().c_str(), O_RDONLY);
    if (parent < 0 || fsync(parent)) { if (parent >= 0) close(parent); stop("cannot synchronize published parent"); }
    close(parent);
#endif
    copy_paths("/dev_hdd0/game/" + pending.name, info, usrdir);
    pending = {};
    return 0;
} catch (const fs::filesystem_error&) { stop("host error while publishing installation");
}
extern "C" int32_t ps3_native_install_string(int32_t id, const char* text) {
    std::lock_guard lock(install_lock);
    if (pending.name.empty()) return failure;
    if (id < 0 || id > 21) stop("unreviewed installation string parameter");
    if (!text || std::strlen(text) >= 128) return param;
    char key[16] = "TITLE";
    if (id >= 2) std::snprintf(key, sizeof key, "TITLE_%02d", id - 2);
    pending.fields[key] = {text, 128};
    return 0;
}

namespace {
int32_t file_error(int error) {
    switch (error) {
    case ENOENT: return (int32_t)CELL_ENOENT;
    case EEXIST: return (int32_t)CELL_EEXIST;
    case EACCES: return (int32_t)CELL_EACCES;
    case ENOSPC: return (int32_t)CELL_ENOSPC;
    case ENOMEM: return (int32_t)CELL_ENOMEM;
    case EMFILE: return (int32_t)CELL_EMFILE;
    case EINVAL: return (int32_t)CELL_EINVAL;
    default: return (int32_t)CELL_EIO;
    }
}
bool file_component(const std::string& value) {
    if (value.empty() || value == "." || value == ".." || value.size() > 255 ||
        value.find_first_of("\\:*?\"<>|") != value.npos || value.back() == '.' || value.back() == ' ') return false;
    std::string stem = value.substr(0, value.find('.'));
    for (char& ch : stem) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$") return false;
    return !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9');
}
}
extern "C" int32_t ps3_native_install_open(const char* guest, uint32_t flags, FILE** result) try {
    std::lock_guard lock(install_lock);
    *result = nullptr;
    const uint32_t access = flags & 3;
    if ((flags & ~0x6C3u) || access == 3) return (int32_t)CELL_EINVAL;
    if (!access || ((flags & 0x80) && !(flags & 0x40))) stop("unreviewed installation open flags");
    const std::string path = guest ? guest : "", prefix = pending.guest + '/';
    if (pending.name.empty() || !path.starts_with(prefix)) {
        std::fprintf(stderr, "[storage] refused write open '%s' flags=0x%X\n", path.c_str(), flags);
        stop("write is outside the active installation");
    }
    fs::path relative;
    for (size_t start = prefix.size(); start < path.size();) {
        auto end = path.find('/', start); if (end == path.npos) end = path.size();
        auto part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (!file_component(part)) stop("invalid installation file component");
            relative /= fs::u8path(part);
        }
        start = end + 1;
    }
    if (relative.empty()) return (int32_t)CELL_EISDIR;
    const auto utf8_leaf = relative.generic_u8string();
    std::string leaf(reinterpret_cast<const char*>(utf8_leaf.data()), utf8_leaf.size());
    for (char& ch : leaf) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (leaf == "PARAM.SFO") stop("installation SFO is owned by the metadata transaction");
    auto host = pending.data / relative;
    if (!contained(pending.data, host)) stop("escaping installation file");
    auto current = pending.root;
    for (const auto& part : host.lexically_relative(pending.root)) {
        current /= part;
        std::error_code ec;
        auto status = fs::symlink_status(current, ec);
        if (!ec && fs::is_symlink(status)) stop("installation path contains a link");
    }
    if (fs::is_directory(host)) return (int32_t)CELL_EISDIR;
    if (fs::exists(host) && (!fs::is_regular_file(host) || fs::hard_link_count(host) != 1)) stop("unsupported installation file or hard link");
    unsigned entries = 0;
    for (const auto& unused : fs::recursive_directory_iterator(pending.data)) {
        (void)unused; if (++entries > 200) stop("installation entry budget exceeded");
    }
    if (entries == 200 && !fs::exists(host)) stop("installation entry budget exhausted");
    int native_flags = access == 2 ? O_RDWR : O_WRONLY;
    if (flags & 0x40) native_flags |= O_CREAT;
    if (flags & 0x80) native_flags |= O_EXCL;
    if (flags & 0x400) native_flags |= O_APPEND;
#ifdef _WIN32
    int descriptor = _wopen(host.c_str(), native_flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(host.c_str(), native_flags | O_NOFOLLOW | O_CLOEXEC, 0666);
#endif
    if (descriptor < 0) return file_error(errno);
    bool valid = false;
#ifdef _WIN32
    HANDLE handle = (HANDLE)_get_osfhandle(descriptor);
    BY_HANDLE_FILE_INFORMATION info;
    std::vector<wchar_t> final_name(32768);
    DWORD length = GetFinalPathNameByHandleW(handle, final_name.data(), (DWORD)final_name.size(), FILE_NAME_NORMALIZED);
    if (length && length < final_name.size() && GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks == 1) {
        std::wstring final(final_name.data(), length);
        if (final.starts_with(L"\\\\?\\UNC\\")) final = L"\\\\" + final.substr(8);
        else if (final.starts_with(L"\\\\?\\")) final.erase(0, 4);
        valid = contained(pending.data, fs::path(final));
    }
    if (!valid) { _close(descriptor); stop("opened handle escapes installation or has links"); }
    if ((flags & 0x200) && _chsize_s(descriptor, 0)) { int error = errno; _close(descriptor); return file_error(error); }
#else
    struct stat info;
    valid = !fstat(descriptor, &info) && S_ISREG(info.st_mode) && info.st_nlink == 1;
    if (!valid) { close(descriptor); stop("opened installation handle has unsupported type or links"); }
    if ((flags & 0x200) && ftruncate(descriptor, 0)) { int error = errno; close(descriptor); return file_error(error); }
#endif
    const char* mode = access == 2 ? "rb+" : "wb";
    FILE* stream = fdopen(descriptor, mode);
    if (!stream) {
        int error = errno;
#ifdef _WIN32
        _close(descriptor);
#else
        close(descriptor);
#endif
        return file_error(error);
    }
    if (setvbuf(stream, nullptr, _IONBF, 0)) { fclose(stream); return (int32_t)CELL_EIO; }
    *result = stream;
    return 0;
} catch (const fs::filesystem_error&) { stop("host error while opening installation file"); }

extern "C" void ps3_native_install_charge(uint64_t requested) {
    std::lock_guard lock(install_lock);
    static uint64_t written;
    if (requested > 1024ull * 1024 * 1024 - written) stop("session write budget of 1 GiB exhausted");
    written += requested;
}
