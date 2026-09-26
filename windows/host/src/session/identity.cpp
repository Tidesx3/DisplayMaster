#include "session/identity.h"

#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>

#include <fstream>
#include <iterator>
#include <vector>

#include "core/log.h"
#include "core/win.h"
#include "monocypher.h"

namespace dm {
namespace {

std::vector<uint8_t> dpapi(const std::vector<uint8_t>& in, bool protect) {
    DATA_BLOB src{static_cast<DWORD>(in.size()), const_cast<BYTE*>(in.data())}, dst{};
    const BOOL ok = protect ? CryptProtectData(&src, L"DisplayMaster identity", nullptr, nullptr, nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN, &dst)
                            : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst);
    if (!ok) return {};
    std::vector<uint8_t> out(dst.pbData, dst.pbData + dst.cbData);
    SecureZeroMemory(dst.pbData, dst.cbData);
    LocalFree(dst.pbData);
    return out;
}

}  // namespace

std::filesystem::path default_identity_path() {
    PWSTR path = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) dir = path;
    CoTaskMemFree(path);
    return dir / L"DisplayMaster" / L"identity.key";
}

noise::KeyPair load_or_create_identity(const std::filesystem::path& path) {
    {
        std::ifstream f(path, std::ios::binary);
        const std::vector<uint8_t> sealed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto priv = sealed.empty() ? std::vector<uint8_t>{} : dpapi(sealed, false);
        if (priv.size() == noise::kKeySize) {
            noise::Key k;
            std::copy(priv.begin(), priv.end(), k.begin());
            crypto_wipe(priv.data(), priv.size());
            auto kp = noise::KeyPair::from_private(k);
            crypto_wipe(k.data(), k.size());
            return kp;
        }
        if (!sealed.empty()) DM_LOGW("Identity key unreadable - creating a new one (devices must pair again)");
    }
    auto kp = noise::KeyPair::generate();
    const auto sealed = dpapi(std::vector<uint8_t>(kp.priv.begin(), kp.priv.end()), true);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(sealed.data()), static_cast<std::streamsize>(sealed.size()));
    if (!f || sealed.empty())
        DM_LOGW("Could not save the identity key to %s - devices will need to pair again after a restart",
                to_utf8(path.wstring()).c_str());
    else
        DM_LOGI("Created this PC's identity key");
    return kp;
}

}  // namespace dm
