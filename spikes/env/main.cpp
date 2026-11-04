#include <bgfx/bgfx.h>
#include <d3d12.h>
#include <dml_provider_factory.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <onnxruntime_cxx_api.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kIntelVendorId = 0x8086;

std::string hresult_message(const HRESULT result) {
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(result);
    return stream.str();
}

void check_hresult(const HRESULT result, const std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed: " + hresult_message(result));
    }
}

std::string utf8(const wchar_t* const value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

OSVERSIONINFOW os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll   = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        throw std::runtime_error("RtlGetVersion is unavailable");
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) != 0) {
        throw std::runtime_error("RtlGetVersion failed");
    }
    return version;
}

struct AdapterSelection {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    std::uint32_t index = 0;
};

AdapterSelection select_intel_adapter() {
    ComPtr<IDXGIFactory6> factory;
    check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    for (std::uint32_t index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check_hresult(result, "IDXGIFactory6::EnumAdapters1");

        DXGI_ADAPTER_DESC1 description{};
        check_hresult(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if (description.VendorId == kIntelVendorId &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            return {std::move(adapter), description, index};
        }
    }

    throw std::runtime_error("No hardware Intel adapter was found");
}

struct WindowGuard {
    HWND handle = nullptr;

    ~WindowGuard() {
        if (handle != nullptr) {
            DestroyWindow(handle);
        }
    }
};

struct BgfxGuard {
    bool initialized = false;

    ~BgfxGuard() {
        if (initialized) {
            bgfx::shutdown();
        }
    }
};

void run_bgfx_gate(ComPtr<ID3D12Device> device, const DXGI_ADAPTER_DESC1& adapter) {
    WindowGuard window{CreateWindowExW(0, L"STATIC", L"SparkFace Phase-0", WS_OVERLAPPEDWINDOW,
                                       CW_USEDEFAULT, CW_USEDEFAULT, 16, 16, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr)};
    if (window.handle == nullptr) {
        throw std::runtime_error("CreateWindowExW failed");
    }

    bgfx::Init init;
    init.type                 = bgfx::RendererType::Direct3D12;
    init.vendorId             = static_cast<std::uint16_t>(adapter.VendorId);
    init.deviceId             = static_cast<std::uint16_t>(adapter.DeviceId);
    init.platformData.nwh     = window.handle;
    init.platformData.context = device.Get();
    init.resolution.width     = 16;
    init.resolution.height    = 16;
    init.resolution.reset     = BGFX_RESET_NONE;

    BgfxGuard guard;
    if (!bgfx::init(init)) {
        throw std::runtime_error("bgfx D3D12 initialization failed");
    }
    device.Reset();
    guard.initialized = true;

    const bgfx::Caps* const caps = bgfx::getCaps();
    if (caps == nullptr || caps->rendererType != bgfx::RendererType::Direct3D12) {
        throw std::runtime_error("bgfx did not select the D3D12 renderer");
    }

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x203040ffU, 1.0F, 0);
    bgfx::setViewRect(0, 0, 0, 1, 1);
    bgfx::touch(0);
    bgfx::frame();
    std::cout << "bgfx_renderer=" << bgfx::getRendererName(bgfx::RendererType::Direct3D12) << '\n';
}

std::filesystem::path gate_model_path() {
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length == static_cast<DWORD>(executable.size())) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"mul_1.onnx";
}

std::size_t element_count(std::vector<std::int64_t>& shape) {
    for (std::int64_t& dimension : shape) {
        if (dimension <= 0) {
            dimension = 1;
        }
    }

    return std::accumulate(shape.begin(), shape.end(), std::size_t{1},
                           [](const std::size_t count, const std::int64_t dimension) {
                               if (static_cast<std::uint64_t>(dimension) >
                                   std::numeric_limits<std::size_t>::max() / count) {
                                   throw std::overflow_error("ONNX tensor shape is too large");
                               }
                               return count * static_cast<std::size_t>(dimension);
                           });
}

void run_directml_gate(const std::uint32_t adapter_index) {
    const std::vector<std::string> providers = Ort::GetAvailableProviders();
    if (std::find(providers.begin(), providers.end(), "DmlExecutionProvider") == providers.end()) {
        throw std::runtime_error("DmlExecutionProvider is unavailable");
    }

    Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "sparkface-env-gate");
    Ort::SessionOptions options;
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.DisableMemPattern();
    options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    Ort::ThrowOnError(
        OrtSessionOptionsAppendExecutionProvider_DML(options, static_cast<int>(adapter_index)));

    const std::filesystem::path model_path = gate_model_path();
    Ort::Session session(environment, model_path.c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;

    const std::size_t input_count  = session.GetInputCount();
    const std::size_t output_count = session.GetOutputCount();
    if (input_count == 0 || output_count == 0) {
        throw std::runtime_error("The DirectML gate model has no inputs or outputs");
    }

    std::vector<Ort::AllocatedStringPtr> input_name_storage;
    std::vector<const char*> input_names;
    std::vector<std::vector<std::int64_t>> input_shapes;
    std::vector<std::vector<float>> input_data;
    input_name_storage.reserve(input_count);
    input_names.reserve(input_count);
    input_shapes.reserve(input_count);
    input_data.reserve(input_count);

    for (std::size_t index = 0; index < input_count; ++index) {
        auto tensor_info = session.GetInputTypeInfo(index).GetTensorTypeAndShapeInfo();
        if (tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("The DirectML gate model input is not float");
        }
        input_shapes.emplace_back(tensor_info.GetShape());
        input_data.emplace_back(element_count(input_shapes.back()), static_cast<float>(index + 2));
        input_name_storage.emplace_back(session.GetInputNameAllocated(index, allocator));
        input_names.push_back(input_name_storage.back().get());
    }

    const Ort::MemoryInfo memory_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> input_tensors;
    input_tensors.reserve(input_count);
    for (std::size_t index = 0; index < input_count; ++index) {
        input_tensors.emplace_back(Ort::Value::CreateTensor<float>(
            memory_info, input_data[index].data(), input_data[index].size(),
            input_shapes[index].data(), input_shapes[index].size()));
    }

    std::vector<Ort::AllocatedStringPtr> output_name_storage;
    std::vector<const char*> output_names;
    output_name_storage.reserve(output_count);
    output_names.reserve(output_count);
    for (std::size_t index = 0; index < output_count; ++index) {
        output_name_storage.emplace_back(session.GetOutputNameAllocated(index, allocator));
        output_names.push_back(output_name_storage.back().get());
    }

    std::vector<Ort::Value> outputs =
        session.Run(Ort::RunOptions{nullptr}, input_names.data(), input_tensors.data(),
                    input_tensors.size(), output_names.data(), output_names.size());
    if (outputs.empty() || !outputs.front().IsTensor()) {
        throw std::runtime_error("DirectML returned no tensor output");
    }

    const auto output_info = outputs.front().GetTensorTypeAndShapeInfo();
    if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        output_info.GetElementCount() == 0) {
        throw std::runtime_error("DirectML returned an invalid output tensor");
    }
    const float first_value = outputs.front().GetTensorData<float>()[0];
    if (!std::isfinite(first_value)) {
        throw std::runtime_error("DirectML returned a non-finite result");
    }

    std::cout << "directml_provider=DmlExecutionProvider\n"
              << "directml_first_output=" << first_value << '\n';
}

void verify_virtual_camera_api() {
    const HMODULE module = LoadLibraryW(L"mfsensorgroup.dll");
    if (module == nullptr) {
        throw std::runtime_error("mfsensorgroup.dll is unavailable");
    }

    const FARPROC create_virtual_camera = GetProcAddress(module, "MFCreateVirtualCamera");
    FreeLibrary(module);
    if (create_virtual_camera == nullptr) {
        throw std::runtime_error("MFCreateVirtualCamera is unavailable");
    }
    std::cout << "virtual_camera_api=MFCreateVirtualCamera\n";
}

void print_build_environment() {
    std::cout << "cmake_version=" << SF_CMAKE_VERSION << '\n'
              << "compiler_version=" << SF_COMPILER_VERSION << '\n'
              << "msvc_full_version=" << _MSC_FULL_VER << '\n'
              << "windows_sdk_version=" << SF_WINDOWS_SDK_VERSION << '\n'
              << "vcpkg_baseline=" << SF_VCPKG_BASELINE << '\n'
              << "bgfx_port_version=" << SF_BGFX_PORT_VERSION << '\n'
              << "onnxruntime_directml_version=" << SF_ORT_DML_VERSION << '\n';
}

} // namespace

int main() {
    bool com_initialized          = false;
    bool media_foundation_started = false;
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try {
        print_build_environment();
        const HRESULT com_result =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        check_hresult(com_result, "CoInitializeEx");
        com_initialized = true;

        check_hresult(MFStartup(MF_VERSION), "MFStartup");
        media_foundation_started = true;

        const OSVERSIONINFOW version = os_version();
        std::cout << "os_version=" << version.dwMajorVersion << '.' << version.dwMinorVersion << '.'
                  << version.dwBuildNumber << '\n';
        if (version.dwBuildNumber < 22000) {
            throw std::runtime_error("Windows build 22000 or newer is required");
        }
        verify_virtual_camera_api();

        const AdapterSelection selected = select_intel_adapter();
        std::cout << "adapter_name=" << utf8(selected.description.Description) << '\n'
                  << "adapter_vendor=0x" << std::hex << selected.description.VendorId
                  << "\nadapter_device=0x" << selected.description.DeviceId << std::dec
                  << "\nadapter_index=" << selected.index << '\n';

        ComPtr<ID3D12Device> device;
        check_hresult(D3D12CreateDevice(selected.adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device)),
                      "D3D12CreateDevice");

        run_bgfx_gate(std::move(device), selected.description);
        run_directml_gate(selected.index);

        MFShutdown();
        media_foundation_started = false;
        CoUninitialize();
        com_initialized = false;

        std::cout << "ENV_GATE_PASS\n";
        return 0;
    } catch (const Ort::Exception& error) {
        std::cerr << "ENV_GATE_FAIL: ONNX Runtime: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "ENV_GATE_FAIL: " << error.what() << '\n';
    }

    if (media_foundation_started) {
        MFShutdown();
    }
    if (com_initialized) {
        CoUninitialize();
    }
    return 1;
}
