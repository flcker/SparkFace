// SPDX-License-Identifier: Apache-2.0
#include "control_protocol.h"

#include <Aclapi.h>
#include <sddl.h>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace SparkFaceSourceLocalControl;

struct Options {
    enum class Mode { Hold, Mismatch, AclProbe } mode = Mode::Hold;
    std::uint32_t value = 50;
    std::uint32_t revision = 1;
    DWORD heartbeatMilliseconds = 100;
    DWORD holdMilliseconds = 0;
};

struct SecurityAttributes final {
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL dacl = nullptr;
    PACL sacl = nullptr;

    ~SecurityAttributes() {
        if (dacl != nullptr) {
            LocalFree(dacl);
        }
        if (sacl != nullptr) {
            LocalFree(sacl);
        }
    }
};

struct Handle final {
    HANDLE value = nullptr;
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle() = default;
    Handle(Handle&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != nullptr && value != INVALID_HANDLE_VALUE) {
                CloseHandle(value);
            }
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
};

struct View final {
    ControlBlock* value = nullptr;
    ~View() {
        if (value != nullptr) {
            UnmapViewOfFile(value);
        }
    }
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    View() = default;
};

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " error=" +
                             std::to_string(GetLastError()));
}

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--mode" && index + 1 < argc) {
            const std::wstring_view mode(argv[++index]);
            if (mode == L"hold") {
                options.mode = Options::Mode::Hold;
            } else if (mode == L"mismatch") {
                options.mode = Options::Mode::Mismatch;
            } else if (mode == L"acl-probe") {
                options.mode = Options::Mode::AclProbe;
            } else {
                throw std::invalid_argument("--mode must be hold, mismatch, or acl-probe");
            }
        } else if (argument == L"--value" && index + 1 < argc) {
            options.value = static_cast<std::uint32_t>(std::stoul(argv[++index]));
        } else if (argument == L"--revision" && index + 1 < argc) {
            options.revision = static_cast<std::uint32_t>(std::stoul(argv[++index]));
        } else if (argument == L"--heartbeat-ms" && index + 1 < argc) {
            options.heartbeatMilliseconds = static_cast<DWORD>(std::stoul(argv[++index]));
        } else if (argument == L"--hold-ms" && index + 1 < argc) {
            options.holdMilliseconds = static_cast<DWORD>(std::stoul(argv[++index]));
        } else {
            throw std::invalid_argument(
                "usage: transport_source_local_control_producer "
                "[--mode hold|mismatch|acl-probe] [--value 0..100] "
                "[--revision N] [--heartbeat-ms N] [--hold-ms N]");
        }
    }
    if (!IsEffectValueValid(options.value) || options.revision == 0 ||
        options.heartbeatMilliseconds == 0) {
        throw std::invalid_argument("Producer values must be valid");
    }
    return options;
}

void set_trustee(EXPLICIT_ACCESSW* entry, PSID sid, DWORD permissions) {
    entry->grfAccessPermissions = permissions;
    entry->grfAccessMode = SET_ACCESS;
    entry->grfInheritance = NO_INHERITANCE;
    entry->Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry->Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry->Trustee.ptstrName = static_cast<LPWSTR>(sid);
}

void grant_local_service_process_query() {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> localService{};
    DWORD localServiceBytes = static_cast<DWORD>(localService.size());
    if (!CreateWellKnownSid(WinLocalServiceSid, nullptr, localService.data(),
                            &localServiceBytes)) {
        fail("CreateWellKnownSid process query");
    }
    EXPLICIT_ACCESSW entry{};
    set_trustee(&entry, localService.data(),
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION);
    PACL existingDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = GetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   &existingDacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) {
        SetLastError(result);
        fail("GetSecurityInfo process");
    }
    PACL updatedDacl = nullptr;
    result = SetEntriesInAclW(1, &entry, existingDacl, &updatedDacl);
    if (result == ERROR_SUCCESS) {
        result = SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT,
                                 DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                 updatedDacl, nullptr);
    }
    if (updatedDacl != nullptr) {
        LocalFree(updatedDacl);
    }
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
    if (result != ERROR_SUCCESS) {
        SetLastError(result);
        fail("SetSecurityInfo process");
    }
}

void build_security_attributes(SecurityAttributes* security) {
    if (!InitializeSecurityDescriptor(&security->descriptor,
                                     SECURITY_DESCRIPTOR_REVISION)) {
        fail("InitializeSecurityDescriptor");
    }

    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
        fail("OpenProcessToken");
    }
    DWORD tokenBytes = 0;
    (void)GetTokenInformation(token.value, TokenUser, nullptr, 0, &tokenBytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenBytes == 0) {
        fail("GetTokenInformation size");
    }
    std::array<BYTE, 512> tokenInfo{};
    if (tokenBytes > tokenInfo.size() ||
        !GetTokenInformation(token.value, TokenUser, tokenInfo.data(),
                             static_cast<DWORD>(tokenInfo.size()), &tokenBytes)) {
        fail("GetTokenInformation user");
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenInfo.data());

    std::array<BYTE, SECURITY_MAX_SID_SIZE> localService{};
    std::array<BYTE, SECURITY_MAX_SID_SIZE> localSystem{};
    std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD localServiceBytes = static_cast<DWORD>(localService.size());
    DWORD localSystemBytes = static_cast<DWORD>(localSystem.size());
    DWORD administratorsBytes = static_cast<DWORD>(administrators.size());
    if (!CreateWellKnownSid(WinLocalServiceSid, nullptr, localService.data(),
                            &localServiceBytes) ||
        !CreateWellKnownSid(WinLocalSystemSid, nullptr, localSystem.data(),
                            &localSystemBytes) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            administrators.data(), &administratorsBytes)) {
        fail("CreateWellKnownSid");
    }

    std::array<EXPLICIT_ACCESSW, 4> entries{};
    set_trustee(&entries[0], user->User.Sid, GENERIC_ALL);
    set_trustee(&entries[1], localService.data(), GENERIC_READ);
    set_trustee(&entries[2], localSystem.data(), GENERIC_READ);
    set_trustee(&entries[3], administrators.data(), GENERIC_READ);
    const DWORD aclResult = SetEntriesInAclW(static_cast<ULONG>(entries.size()),
                                             entries.data(), nullptr,
                                             &security->dacl);
    if (aclResult != ERROR_SUCCESS) {
        SetLastError(aclResult);
        fail("SetEntriesInAcl");
    }
    if (!SetSecurityDescriptorDacl(&security->descriptor, TRUE, security->dacl,
                                   FALSE)) {
        fail("SetSecurityDescriptorDacl");
    }

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID mediumSid = nullptr;
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_MEDIUM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &mediumSid)) {
        fail("AllocateAndInitializeSid medium");
    }
    const DWORD saclBytes = static_cast<DWORD>(
        sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) +
        GetLengthSid(mediumSid) - sizeof(DWORD));
    security->sacl = static_cast<PACL>(LocalAlloc(LPTR, saclBytes));
    if (security->sacl == nullptr ||
        !InitializeAcl(security->sacl, saclBytes, ACL_REVISION) ||
        !AddMandatoryAce(security->sacl, ACL_REVISION, 0,
                         SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, mediumSid)) {
        FreeSid(mediumSid);
        fail("AddMandatoryAce");
    }
    FreeSid(mediumSid);
    if (!SetSecurityDescriptorSacl(&security->descriptor, TRUE, security->sacl,
                                   FALSE)) {
        fail("SetSecurityDescriptorSacl");
    }
    security->attributes.nLength = sizeof(security->attributes);
    security->attributes.lpSecurityDescriptor = &security->descriptor;
    security->attributes.bInheritHandle = FALSE;
}

Handle create_mapping(SecurityAttributes* security) {
    Handle mapping;
    mapping.value = CreateFileMappingW(
        INVALID_HANDLE_VALUE, &security->attributes, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(ControlBlock)), MappingName);
    if (mapping.value == nullptr) {
        fail("CreateFileMapping");
    }
    return mapping;
}

bool denied_under_token(HANDLE token) {
    if (!ImpersonateLoggedOnUser(token)) {
        fail("ImpersonateLoggedOnUser");
    }
    SetLastError(ERROR_SUCCESS);
    Handle mapping;
    mapping.value = OpenFileMappingW(FILE_MAP_WRITE, FALSE, MappingName);
    const DWORD openError = GetLastError();
    if (!RevertToSelf()) {
        fail("RevertToSelf");
    }
    return mapping.value == nullptr && openError == ERROR_ACCESS_DENIED;
}

bool restricted_write_is_denied(PSID userSid, bool lowIntegrity) {
    SID_AND_ATTRIBUTES disabled{};
    disabled.Sid = userSid;
    Handle current;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                          &current.value)) {
        fail("OpenProcessToken restricted");
    }
    Handle restricted;
    if (!CreateRestrictedToken(current.value, 0, 1, &disabled, 0, nullptr, 0,
                               nullptr, &restricted.value)) {
        fail("CreateRestrictedToken");
    }
    Handle impersonation;
    if (!DuplicateTokenEx(restricted.value,
                          TOKEN_ADJUST_DEFAULT | TOKEN_IMPERSONATE | TOKEN_QUERY,
                          nullptr, SecurityImpersonation, TokenImpersonation,
                          &impersonation.value)) {
        fail("DuplicateTokenEx");
    }
    if (lowIntegrity) {
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID lowSid = nullptr;
        if (!AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_LOW_RID,
                                      0, 0, 0, 0, 0, 0, 0, &lowSid)) {
            fail("AllocateAndInitializeSid low");
        }
        TOKEN_MANDATORY_LABEL label{};
        label.Label.Sid = lowSid;
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        const DWORD bytes =
            static_cast<DWORD>(sizeof(label) + GetLengthSid(lowSid));
        const BOOL set = SetTokenInformation(impersonation.value,
                                             TokenIntegrityLevel, &label, bytes);
        FreeSid(lowSid);
        if (!set) {
            fail("SetTokenInformation low integrity");
        }
    }
    return denied_under_token(impersonation.value);
}

struct NonAdminProbe {
    bool isNonAdmin = false;
    bool writeAllowed = false;
};

NonAdminProbe non_admin_write_probe() {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD administratorBytes = static_cast<DWORD>(administrators.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            administrators.data(), &administratorBytes)) {
        fail("CreateWellKnownSid non-admin");
    }
    SID_AND_ATTRIBUTES disabled{};
    disabled.Sid = administrators.data();
    Handle current;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                          &current.value)) {
        fail("OpenProcessToken non-admin");
    }
    Handle restricted;
    if (!CreateRestrictedToken(current.value, 0, 1, &disabled, 0, nullptr, 0,
                               nullptr, &restricted.value)) {
        fail("CreateRestrictedToken non-admin");
    }
    Handle impersonation;
    if (!DuplicateTokenEx(restricted.value,
                          TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr,
                          SecurityImpersonation, TokenImpersonation,
                          &impersonation.value)) {
        fail("DuplicateTokenEx non-admin");
    }
    if (!ImpersonateLoggedOnUser(impersonation.value)) {
        fail("ImpersonateLoggedOnUser non-admin");
    }
    BOOL isAdministrator = FALSE;
    const BOOL checked = CheckTokenMembership(nullptr, administrators.data(),
                                              &isAdministrator);
    Handle mapping;
    mapping.value = OpenFileMappingW(FILE_MAP_WRITE, FALSE, MappingName);
    const BOOL reverted = RevertToSelf();
    if (!checked || !reverted) {
        fail("CheckTokenMembership non-admin");
    }
    return {!isAdministrator, mapping.value != nullptr};
}

PSID current_user_sid(std::array<BYTE, 512>* tokenInfo) {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
        fail("OpenProcessToken current user");
    }
    DWORD bytes = 0;
    if (!GetTokenInformation(token.value, TokenUser, tokenInfo->data(),
                             static_cast<DWORD>(tokenInfo->size()), &bytes)) {
        fail("GetTokenInformation current user");
    }
    return reinterpret_cast<TOKEN_USER*>(tokenInfo->data())->User.Sid;
}

std::int64_t qpc_now() {
    LARGE_INTEGER qpc{};
    if (!QueryPerformanceCounter(&qpc)) {
        fail("QueryPerformanceCounter");
    }
    return qpc.QuadPart;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parse_options(argc, argv);
        grant_local_service_process_query();
        SecurityAttributes security;
        build_security_attributes(&security);
        Handle mapping = create_mapping(&security);
        View view;
        view.value = static_cast<ControlBlock*>(
            MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, sizeof(ControlBlock)));
        if (view.value == nullptr) {
            fail("MapViewOfFile");
        }

        if (options.mode == Options::Mode::AclProbe) {
            std::array<BYTE, 512> tokenInfo{};
            PSID userSid = current_user_sid(&tokenInfo);
            const bool restrictedDenied = restricted_write_is_denied(userSid, false);
            const bool lowDenied = restricted_write_is_denied(userSid, true);
            const NonAdminProbe nonAdmin = non_admin_write_probe();
            std::cout << "control_acl_restricted_write_denied="
                      << (restrictedDenied ? 1 : 0) << '\n'
                      << "control_acl_low_integrity_write_denied="
                      << (lowDenied ? 1 : 0) << '\n'
                      << "control_acl_non_admin="
                      << (nonAdmin.isNonAdmin ? 1 : 0) << '\n'
                      << "control_acl_non_admin_write_open="
                      << (nonAdmin.writeAllowed ? 1 : 0) << '\n';
            return restrictedDenied && lowDenied && nonAdmin.isNonAdmin &&
                           nonAdmin.writeAllowed
                       ? 0
                       : 2;
        }

        const bool mismatch = options.mode == Options::Mode::Mismatch;
        const ULONGLONG started = GetTickCount64();
        do {
            Snapshot snapshot{};
            snapshot.revision = options.revision;
            snapshot.producerPid = GetCurrentProcessId();
            snapshot.heartbeatQpc = qpc_now();
            snapshot.flags = 1;
            snapshot.effectValue = options.value;
            Publish(view.value, snapshot, mismatch);
            std::cout << "control_publish_revision=" << snapshot.revision << '\n'
                      << "control_publish_value=" << snapshot.effectValue << '\n'
                      << "control_protocol_mismatch=" << (mismatch ? 1 : 0)
                      << std::endl;
            Sleep(options.heartbeatMilliseconds);
        } while (options.holdMilliseconds == 0 ||
                 GetTickCount64() - started < options.holdMilliseconds);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SOURCE_LOCAL_CONTROL_PRODUCER_ERROR: " << error.what()
                  << '\n';
        return 1;
    }
}
