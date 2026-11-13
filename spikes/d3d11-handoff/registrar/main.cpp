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

// Reuse the retained B2 media-source COM identity; the runner restores its
// original DLL registration after every Spike D attempt.
constexpr GUID kSourceClsid{0xa1d6a9e3, 0x6d95, 0x4da8,
                            {0xab, 0x2e, 0x98, 0x7b, 0x53, 0x22, 0xf8, 0x61}};
constexpr wchar_t kSourceClsidText[] = L"{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}";
constexpr wchar_t kFriendlyName[] = L"SparkFace Phase0 D Camera";
constexpr wchar_t kComKey[] =
    L"Software\\Classes\\CLSID\\{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}\\InprocServer32";

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
    std::wcout << L"camera_present=" << (found ? L"yes" : L"no") << L'\n';
}

void probe_media_source(const std::filesystem::path& dll_path) {
    const HMODULE module = LoadLibraryW(std::filesystem::absolute(dll_path).c_str());
    if (module == nullptr) {
        check_win32(GetLastError(), "LoadLibraryW");
    }
    using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto get_class_object = reinterpret_cast<DllGetClassObjectFunction>(
        GetProcAddress(module, "DllGetClassObject"));
    if (get_class_object == nullptr) {
        const LSTATUS error = GetLastError();
        FreeLibrary(module);
        check_win32(error, "GetProcAddress(DllGetClassObject)");
    }
    ComPtr<IClassFactory> factory;
    const HRESULT factory_result =
        get_class_object(kSourceClsid, IID_PPV_ARGS(&factory));
    if (FAILED(factory_result)) {
        FreeLibrary(module);
        check_hresult(factory_result, "DllGetClassObject");
    }
    ComPtr<IUnknown> instance;
    const HRESULT instance_result = factory->CreateInstance(nullptr, IID_PPV_ARGS(&instance));
    instance.Reset();
    factory.Reset();
    FreeLibrary(module);
    check_hresult(instance_result, "IClassFactory::CreateInstance");
    std::cout << "media_source_activation=pass\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) {
            throw std::runtime_error(
                "Usage: d3d11_handoff_registrar <install DLL|remove|list|probe DLL>");
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
        } else if (command == L"probe") {
            if (argc != 3) {
                throw std::runtime_error("probe requires the media-source DLL path");
            }
            probe_media_source(argv[2]);
        } else {
            throw std::runtime_error("Unknown registrar command");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "registrar_error=" << error.what() << '\n';
        return 1;
    }
}
