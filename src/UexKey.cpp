#include "UexReports.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "AppPaths.h"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace SCMiner {
namespace UexKey {

namespace {
std::filesystem::path where() { return AppPaths::data() / FILE_NAME; }
}  // namespace

bool save(const std::string& key) {
    std::string sealed;
#ifdef _WIN32
    DATA_BLOB in{(DWORD)key.size(), (BYTE*)const_cast<char*>(key.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Bedrock UEX key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    sealed.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
#else
    sealed = key;
#endif
    const std::filesystem::path file = where();
    const std::filesystem::path fresh = file.string() + ".new";
    {
        std::ofstream write(fresh, std::ios::binary | std::ios::trunc);
        if (!write.is_open()) return false;
        write.write(sealed.data(), (std::streamsize)sealed.size());
        if (!write) return false;
    }
    std::error_code ec;
#ifndef _WIN32
    // the owner alone may read it
    std::filesystem::permissions(fresh, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
#endif
    std::filesystem::rename(fresh, file, ec);
    return !ec;
}

std::string load() {
    std::ifstream read(where(), std::ios::binary);
    if (!read.is_open()) return "";
    const std::string sealed((std::istreambuf_iterator<char>(read)), std::istreambuf_iterator<char>());
    if (sealed.empty()) return "";
#ifdef _WIN32
    DATA_BLOB in{(DWORD)sealed.size(), (BYTE*)const_cast<char*>(sealed.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string key((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key;
#else
    return sealed;
#endif
}

void forget() {
    std::error_code ec;
    std::filesystem::remove(where(), ec);
}

}  // namespace UexKey
}  // namespace SCMiner
