// SPDX-License-Identifier: Apache-2.0

#include <Windows.h>
#include <objbase.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using Microsoft::WRL::ComPtr;

constexpr GUID SourceClsid{0xa1d6a9e3, 0x6d95, 0x4da8,
                           {0xab, 0x2e, 0x98, 0x7b, 0x53, 0x22, 0xf8, 0x61}};

struct Options {
    std::filesystem::path dll;
    std::filesystem::path ready;
    std::filesystem::path stop;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--dll" && index + 1 < argc) {
            options.dll = argv[++index];
        } else if (argument == L"--ready" && index + 1 < argc) {
            options.ready = argv[++index];
        } else if (argument == L"--stop" && index + 1 < argc) {
            options.stop = argv[++index];
        } else if (argument != L"-Embedding") {
            throw std::invalid_argument(
                "usage: transport_activation_local_server --dll PATH --ready PATH --stop PATH");
        }
    }
    if (options.dll.empty() || options.ready.empty() || options.stop.empty()) {
        throw std::invalid_argument("--dll, --ready, and --stop are required");
    }
    return options;
}

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
}

class ComApartment final {
  public:
    ComApartment() { check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComApartment() { CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

class Module final {
  public:
    explicit Module(const std::filesystem::path& path) : value_(LoadLibraryW(path.c_str())) {
        if (value_ == nullptr) {
            throw std::runtime_error("LoadLibraryW failed: " +
                                     std::to_string(GetLastError()));
        }
    }
    ~Module() { FreeLibrary(value_); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    [[nodiscard]] HMODULE get() const { return value_; }

  private:
    HMODULE value_ = nullptr;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = parse_options(argc, argv);
        std::error_code error;
        std::filesystem::remove(options.ready, error);
        ComApartment apartment;
        Module module(options.dll);
        using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
        const auto getClassObject = reinterpret_cast<DllGetClassObjectFunction>(
            GetProcAddress(module.get(), "DllGetClassObject"));
        if (getClassObject == nullptr) {
            throw std::runtime_error("DllGetClassObject export is missing");
        }
        ComPtr<IClassFactory> factory;
        check_hresult(getClassObject(SourceClsid, IID_PPV_ARGS(&factory)),
                      "DllGetClassObject");
        DWORD registration = 0;
        check_hresult(CoRegisterClassObject(
                          SourceClsid, factory.Get(), CLSCTX_LOCAL_SERVER,
                          REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &registration),
                      "CoRegisterClassObject");
        check_hresult(CoResumeClassObjects(), "CoResumeClassObjects");
        {
            std::ofstream ready(options.ready, std::ios::trunc);
            ready << "pid=" << GetCurrentProcessId() << '\n';
        }
        std::cout << "local_server_pid=" << GetCurrentProcessId() << '\n'
                  << "LOCAL_SERVER_READY\n";
        while (!std::filesystem::exists(options.stop)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check_hresult(CoRevokeClassObject(registration), "CoRevokeClassObject");
        std::cout << "LOCAL_SERVER_STOPPED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LOCAL_SERVER_ERROR: " << error.what() << '\n';
        return 1;
    }
}
