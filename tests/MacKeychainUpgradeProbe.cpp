// Standalone macOS platform regression probe. Uses the same noninteractive
// generic-password API as CredentialStore, but ONLY in a disposable keychain.
// No Qt app startup, real credential service, default-keychain lookup, or SSH.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NOXSHELL_UPGRADE_BUILD
#define NOXSHELL_UPGRADE_BUILD "A"
#endif

namespace {
// Public fixture values, not secrets. Kept out of process arguments and output.
constexpr char kKeychainPassword[] = "noxshell-disposable-upgrade-test";
constexpr char kPassword[] = "synthetic-ssh-password-not-a-real-credential";
constexpr char kPrefix[] = "/private/tmp/noxshell-keychain-upgrade.";

template <typename T> struct Owned {
    T value = nullptr;
    ~Owned() { if (value) CFRelease(value); }
    Owned() = default;
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
};

int report(const char *operation, OSStatus status)
{
    std::printf("build=%s %s status=%d\n", NOXSHELL_UPGRADE_BUILD, operation, int(status));
    return status == errSecSuccess ? 0 : 1;
}

bool isDisposablePath(const std::string &path)
{
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos || path.substr(separator + 1) != "credentials.keychain-db") return false;
    char *resolved = realpath(path.substr(0, separator).c_str(), nullptr);
    if (!resolved) return false;
    const std::string parent(resolved);
    free(resolved);
    if (parent.rfind(kPrefix, 0) != 0 || parent.find('/', strlen(kPrefix)) != std::string::npos) return false;
    struct stat directory{}, item{};
    if (stat(parent.c_str(), &directory) || !S_ISDIR(directory.st_mode)
        || directory.st_uid != geteuid() || (directory.st_mode & 0077)) return false;
    return lstat(path.c_str(), &item) != 0 || (S_ISREG(item.st_mode) && item.st_uid == geteuid());
}
}

int main(int argc, char **argv)
{
    if (argc != 3 || !isDisposablePath(argv[2])) return 2;
    OSStatus status = SecKeychainSetUserInteractionAllowed(false);
    if (status != errSecSuccess) return report("disable-ui", status);
    const std::string action(argv[1]);
    Owned<SecKeychainRef> keychain;
    if (action == "create") {
        Owned<CFArrayRef> searchList;
        status = SecKeychainCopySearchList(&searchList.value);
        if (status != errSecSuccess) return report("read-search-list", status);
        // SecKeychainCreate can add the new keychain to the user's search list.
        // Restore it immediately; never change the default keychain.
        status = SecKeychainCreate(argv[2], strlen(kKeychainPassword), kKeychainPassword,
            false, nullptr, &keychain.value);
        const auto restored = SecKeychainSetSearchList(searchList.value);
        if (restored != errSecSuccess) return report("restore-search-list", restored);
        return report("create", status);
    }
    status = SecKeychainOpen(argv[2], &keychain.value);
    if (status != errSecSuccess) return report("open", status);
    if (action == "delete") return report("delete", SecKeychainDelete(keychain.value));
    status = SecKeychainUnlock(keychain.value, strlen(kKeychainPassword), kKeychainPassword, true);
    if (status != errSecSuccess) return report("unlock-test-keychain", status);

    Owned<CFMutableDictionaryRef> query;
    query.value = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query.value, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.value, kSecAttrService, CFSTR("com.noxshell.test.keychain-upgrade"));
    CFDictionarySetValue(query.value, kSecAttrAccount, CFSTR("synthetic-only"));
    CFDictionarySetValue(query.value, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    if (action == "write") {
        CFDictionarySetValue(query.value, kSecUseKeychain, keychain.value);
        Owned<CFDataRef> data;
        data.value = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(kPassword), strlen(kPassword));
        CFDictionarySetValue(query.value, kSecValueData, data.value);
        return report("write", SecItemAdd(query.value, nullptr));
    }
    if (action != "read" && action != "read-denied") return 2;
    const void *keychains[] = {keychain.value};
    Owned<CFArrayRef> searchList;
    searchList.value = CFArrayCreate(nullptr, keychains, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(query.value, kSecMatchSearchList, searchList.value);
    CFDictionarySetValue(query.value, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.value, kSecMatchLimit, kSecMatchLimitOne);
    Owned<CFTypeRef> result;
    status = SecItemCopyMatching(query.value, &result.value);
    if (action == "read-denied") {
        report("read-untrusted", status);
        // Not-found/invalid-query are NOT proof of access protection.
        return status == errSecAuthFailed || status == errSecInteractionNotAllowed ? 0 : 1;
    }
    if (status == errSecSuccess && (!result.value || CFGetTypeID(result.value) != CFDataGetTypeID()
        || CFDataGetLength(static_cast<CFDataRef>(result.value)) != strlen(kPassword)
        || memcmp(CFDataGetBytePtr(static_cast<CFDataRef>(result.value)), kPassword, strlen(kPassword)))) {
        status = errSecDecode;
    }
    return report("read", status);
}
