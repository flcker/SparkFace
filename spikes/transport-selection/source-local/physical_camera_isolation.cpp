// SPDX-License-Identifier: Apache-2.0
#include "PhysicalCameraCapture.h"

#include <Windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using SparkFaceSourceLocal::PhysicalCameraCapture;

struct Options {
    std::uint64_t frames = 1000;
    std::wstring cameraName = L"Integrated Camera";
};

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " hresult=" +
                                 std::to_string(static_cast<std::uint32_t>(result)));
    }
}

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--frames" && index + 1 < argc) {
            options.frames = std::stoull(argv[++index]);
        } else if (argument == L"--camera-name" && index + 1 < argc) {
            options.cameraName = argv[++index];
        } else {
            throw std::invalid_argument(
                "usage: transport_physical_camera_isolation [--frames N] "
                "[--camera-name NAME]");
        }
    }
    if (options.frames == 0 || options.cameraName.empty()) {
        throw std::invalid_argument("Isolation options must be valid");
    }
    return options;
}

class Runtime final {
  public:
    Runtime() {
        check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED),
                      "CoInitializeEx");
        try {
            check_hresult(MFStartup(MF_VERSION), "MFStartup");
            mediaFoundationStarted_ = true;
        } catch (...) {
            CoUninitialize();
            throw;
        }
    }

    ~Runtime() {
        if (mediaFoundationStarted_) {
            MFShutdown();
        }
        CoUninitialize();
    }

  private:
    bool mediaFoundationStarted_ = false;
};

struct CameraDevice {
    std::wstring name;
    std::wstring symbolicLink;
};

CameraDevice select_camera(const std::wstring& requestedName) {
    ComPtr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    check_hresult(attributes->SetGUID(
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                  "Set source type");
    IMFActivate** rawDevices = nullptr;
    UINT32 count = 0;
    check_hresult(MFEnumDeviceSources(attributes.Get(), &rawDevices, &count),
                  "MFEnumDeviceSources");

    std::optional<CameraDevice> selected;
    for (UINT32 index = 0; index < count; ++index) {
        wchar_t* rawName = nullptr;
        wchar_t* rawLink = nullptr;
        UINT32 nameLength = 0;
        UINT32 linkLength = 0;
        const HRESULT nameResult = rawDevices[index]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &rawName, &nameLength);
        const HRESULT linkResult = rawDevices[index]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &rawLink,
            &linkLength);
        if (SUCCEEDED(nameResult) && SUCCEEDED(linkResult) && rawName != nullptr &&
            rawLink != nullptr && std::wstring_view(rawName) == requestedName) {
            if (selected.has_value()) {
                CoTaskMemFree(rawName);
                CoTaskMemFree(rawLink);
                for (UINT32 remaining = index; remaining < count; ++remaining) {
                    rawDevices[remaining]->Release();
                }
                CoTaskMemFree(rawDevices);
                throw std::runtime_error("Physical camera selection is ambiguous");
            }
            selected = CameraDevice{std::wstring(rawName, nameLength),
                                    std::wstring(rawLink, linkLength)};
        }
        CoTaskMemFree(rawName);
        CoTaskMemFree(rawLink);
        rawDevices[index]->Release();
    }
    CoTaskMemFree(rawDevices);
    if (!selected.has_value() || selected->symbolicLink.empty()) {
        throw std::runtime_error("Requested physical camera was not found");
    }
    return *selected;
}

ComPtr<ID3D11Device> create_device(bool* debugLayer) {
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                            D3D_FEATURE_LEVEL_11_0};
    constexpr UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selectedLevel{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        baseFlags | D3D11_CREATE_DEVICE_DEBUG, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device,
        &selectedLevel, &context);
    *debugLayer = SUCCEEDED(result);
    if (FAILED(result)) {
        device.Reset();
        context.Reset();
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, baseFlags, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device,
            &selectedLevel, &context);
    }
    check_hresult(result, "D3D11CreateDevice");
    ComPtr<ID3D10Multithread> multithread;
    check_hresult(device.As(&multithread), "ID3D10Multithread");
    multithread->SetMultithreadProtected(TRUE);
    return device;
}

double qpc_seconds(std::int64_t begin, std::int64_t end) {
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || end <= begin) {
        throw std::runtime_error("Invalid QPC interval");
    }
    return static_cast<double>(end - begin) /
           static_cast<double>(frequency.QuadPart);
}

std::uint32_t measure_luma_range(ID3D11Device* device,
                                 const PhysicalCameraCapture::Frame& frame) {
    D3D11_TEXTURE2D_DESC sourceDescription{};
    frame.texture()->GetDesc(&sourceDescription);
    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    check_hresult(device->CreateTexture2D(&stagingDescription, nullptr, &staging),
                  "CreateTexture2D camera readback");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture(),
                                   frame.subresource(), nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                  "Map camera readback");
    BYTE minimum = 255;
    BYTE maximum = 0;
    const auto* bytes = static_cast<const BYTE*>(mapped.pData);
    for (std::uint32_t y = 100; y < sourceDescription.Height; y += 200) {
        const BYTE* row = bytes + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (std::uint32_t x = 100; x < sourceDescription.Width; x += 200) {
            const BYTE* bgra = row + static_cast<std::size_t>(x) * 4;
            const BYTE luma = static_cast<BYTE>(
                (29U * bgra[0] + 150U * bgra[1] + 77U * bgra[2]) >> 8);
            minimum = luma < minimum ? luma : minimum;
            maximum = luma > maximum ? luma : maximum;
        }
    }
    context->Unmap(staging.Get(), 0);
    return static_cast<std::uint32_t>(maximum - minimum);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parse_options(argc, argv);
        Runtime runtime;
        const CameraDevice camera = select_camera(options.cameraName);
        bool debugLayer = false;
        ComPtr<ID3D11Device> device = create_device(&debugLayer);

        PhysicalCameraCapture capture;
        check_hresult(capture.Initialize(
                          device.Get(), camera.symbolicLink.c_str(),
                          static_cast<UINT32>(camera.symbolicLink.size()), 1920,
                          1080, 30),
                      "PhysicalCameraCapture::Initialize");

        LARGE_INTEGER started{};
        check_hresult(QueryPerformanceCounter(&started)
                          ? S_OK
                          : HRESULT_FROM_WIN32(GetLastError()),
                      "QueryPerformanceCounter start");
        std::uint64_t firstSequence = 0;
        std::uint64_t lastSequence = 0;
        std::uint64_t skipped = 0;
        std::uint64_t received = 0;
        std::uint32_t minimumSourceLumaRange = 255;
        std::uint32_t maximumSourceLumaRange = 0;
        std::uint64_t sourceContentSamples = 0;
        std::uint64_t lowSourceContentSamples = 0;
        while (received < options.frames) {
            PhysicalCameraCapture::Frame frame;
            const HRESULT result = capture.AcquireNext(&frame);
            if (result == E_PENDING) {
                continue;
            }
            check_hresult(result, "PhysicalCameraCapture::AcquireNext");
            if (frame.texture() == nullptr || frame.capture_qpc() <= 0) {
                throw std::runtime_error("Physical camera frame is incomplete");
            }
            if (received < 5 || ((received + 1) % 100) == 0) {
                const std::uint32_t range =
                    measure_luma_range(device.Get(), frame);
                minimumSourceLumaRange =
                    range < minimumSourceLumaRange ? range
                                                   : minimumSourceLumaRange;
                maximumSourceLumaRange =
                    range > maximumSourceLumaRange ? range
                                                   : maximumSourceLumaRange;
                ++sourceContentSamples;
                if (range < 8) {
                    ++lowSourceContentSamples;
                }
            }
            if (firstSequence == 0) {
                firstSequence = frame.sequence();
            }
            if (lastSequence != 0 && frame.sequence() > lastSequence + 1) {
                skipped += frame.sequence() - lastSequence - 1;
            }
            if (lastSequence != 0 && frame.sequence() <= lastSequence) {
                throw std::runtime_error("Physical camera sequence is not increasing");
            }
            lastSequence = frame.sequence();
            frame.Reset();
            ++received;
        }
        LARGE_INTEGER finished{};
        check_hresult(QueryPerformanceCounter(&finished)
                          ? S_OK
                          : HRESULT_FROM_WIN32(GetLastError()),
                      "QueryPerformanceCounter finish");
        const double seconds = qpc_seconds(started.QuadPart, finished.QuadPart);
        const double fps = static_cast<double>(options.frames) / seconds;
        const HRESULT removedReason = device->GetDeviceRemovedReason();
        const bool pass = fps >= 29.5 && skipped == 0 &&
                          firstSequence != 0 &&
                          lastSequence - firstSequence + 1 == options.frames &&
                          removedReason == S_OK;

        std::cout << std::fixed << std::setprecision(3)
                  << "isolation_camera=Integrated Camera\n"
                  << "isolation_frames=" << options.frames << '\n'
                  << "isolation_fps=" << fps << '\n'
                  << "isolation_first_sequence=" << firstSequence << '\n'
                  << "isolation_last_sequence=" << lastSequence << '\n'
                  << "isolation_skipped_sequences=" << skipped << '\n'
                  << "isolation_source_luma_range_min="
                  << minimumSourceLumaRange << '\n'
                  << "isolation_source_luma_range_max="
                  << maximumSourceLumaRange << '\n'
                  << "isolation_source_content_samples=" << sourceContentSamples
                  << '\n'
                  << "isolation_low_source_content_samples="
                  << lowSourceContentSamples << '\n'
                  << "isolation_debug_layer=" << (debugLayer ? 1 : 0) << '\n'
                  << "isolation_device_removed_reason=0x" << std::hex
                  << std::uppercase << static_cast<std::uint32_t>(removedReason)
                  << std::dec << '\n'
                  << (pass ? "PHYSICAL_CAMERA_ISOLATION_PASS\n"
                           : "PHYSICAL_CAMERA_ISOLATION_FAIL\n");
        return pass ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "PHYSICAL_CAMERA_ISOLATION_ERROR: " << error.what() << '\n';
        return 1;
    }
}
