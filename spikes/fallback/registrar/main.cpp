// SPDX-License-Identifier: Apache-2.0
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace {

// {A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}
constexpr GUID kSourceClsid{0xa1d6a9e3, 0x6d95, 0x4da8,
                            {0xab, 0x2e, 0x98, 0x7b, 0x53, 0x22, 0xf8, 0x61}};
constexpr wchar_t kSourceClsidText[] = L"{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}";
constexpr wchar_t kFriendlyName[] = L"SparkFace Phase0 B2 Camera";
constexpr wchar_t kComKey[] =
    L"Software\\Classes\\CLSID\\{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}\\InprocServer32";
constexpr wchar_t kRingFilePath[] = L"C:\\Users\\Public\\SparkFacePhase0B2Ring.bin";

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
}

void check_win32(LSTATUS result, const char* operation) {
    if (result != ERROR_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with Win32 error " +
                                 std::to_string(result));
    }
}

class MfGuard {
  public:
    MfGuard() { check_hresult(MFStartup(MF_VERSION), "MFStartup"); }
    ~MfGuard() { MFShutdown(); }
};

void register_com_server(const std::filesystem::path& dll_path) {
    HKEY raw_key = nullptr;
    check_win32(RegCreateKeyExW(HKEY_LOCAL_MACHINE, kComKey, 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &raw_key, nullptr),
                "RegCreateKeyExW");
    const auto close_key = [&] { RegCloseKey(raw_key); };
    const std::wstring path = std::filesystem::absolute(dll_path).wstring();
    const LSTATUS default_result =
        RegSetValueExW(raw_key, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path.c_str()),
                       static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    if (default_result != ERROR_SUCCESS) {
        close_key();
        check_win32(default_result, "RegSetValueExW(default)");
    }
    constexpr wchar_t threading_model[] = L"Both";
    const LSTATUS threading_result =
        RegSetValueExW(raw_key, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(threading_model),
                       sizeof(threading_model));
    close_key();
    check_win32(threading_result, "RegSetValueExW(ThreadingModel)");
}

ComPtr<IMFVirtualCamera> open_virtual_camera() {
    ComPtr<IMFVirtualCamera> camera;
    check_hresult(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                        MFVirtualCameraLifetime_System,
                                        MFVirtualCameraAccess_AllUsers, kFriendlyName,
                                        kSourceClsidText, nullptr, 0, &camera),
                  "MFCreateVirtualCamera");
    return camera;
}

void install(const std::filesystem::path& dll_path) {
    if (!std::filesystem::is_regular_file(dll_path)) {
        throw std::runtime_error("Media-source DLL does not exist: " + dll_path.string());
    }
    register_com_server(dll_path);
    auto camera = open_virtual_camera();
    check_hresult(camera->Start(nullptr), "IMFVirtualCamera::Start");
    check_hresult(camera->Shutdown(), "IMFVirtualCamera::Shutdown");
    std::wcout << L"installed=" << kFriendlyName << L'\n';
}

void remove() {
    auto camera = open_virtual_camera();
    check_hresult(camera->Remove(), "IMFVirtualCamera::Remove");
    camera.Reset();
    const LSTATUS result = RegDeleteTreeW(
        HKEY_LOCAL_MACHINE,
        L"Software\\Classes\\CLSID\\{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}");
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        check_win32(result, "RegDeleteTreeW");
    }
    for (std::uint32_t attempt = 0; attempt < 50; ++attempt) {
        if (DeleteFileW(kRingFilePath)) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            break;
        }
        if (error != ERROR_SHARING_VIOLATION || attempt == 49) {
            check_win32(static_cast<LSTATUS>(error), "DeleteFileW");
        }
        Sleep(100);
    }
    std::wcout << L"removed=" << kFriendlyName << L'\n';
}

void list() {
    ComPtr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    check_hresult(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                  "IMFAttributes::SetGUID");
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    check_hresult(MFEnumDeviceSources(attributes.Get(), &activations, &count),
                  "MFEnumDeviceSources");
    bool found = false;
    for (UINT32 index = 0; index < count; ++index) {
        wchar_t* name = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) {
            std::wcout << L"camera_name=" << name << L'\n';
            if (std::wstring_view(name).find(kFriendlyName) == 0) {
                found = true;
                wchar_t* symbolic_link = nullptr;
                UINT32 symbolic_link_length = 0;
                if (SUCCEEDED(activations[index]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        &symbolic_link, &symbolic_link_length))) {
                    std::wcout << L"camera_symbolic_link=" << symbolic_link << L'\n';
                    CoTaskMemFree(symbolic_link);
                }
            }
            CoTaskMemFree(name);
        }
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    std::cout << "camera_present=" << (found ? "yes" : "no") << '\n';
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) {
            throw std::runtime_error("Usage: fallback_registrar <install DLL|remove|list>");
        }
        MfGuard media_foundation;
        const std::wstring_view command(argv[1]);
        if (command == L"install") {
            if (argc != 3) {
                throw std::runtime_error("install requires the media-source DLL path");
            }
            install(argv[2]);
        } else if (command == L"remove") {
            remove();
        } else if (command == L"list") {
            list();
        } else {
            throw std::runtime_error("Unknown registrar command");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "registrar_error=" << error.what() << '\n';
        return 1;
    }
}
