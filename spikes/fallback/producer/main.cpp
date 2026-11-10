// SPDX-License-Identifier: Apache-2.0
#include "frame_transport.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kIntelVendorId = 0x8086;
constexpr std::uint32_t kWarmupFrames = 30;
constexpr double kFrameBudgetMs = 1000.0 / sparkface::fallback::kFrameRate;

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed: 0x" +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
}

std::int64_t qpc_now() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double qpc_ms(std::int64_t start, std::int64_t end) {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return (static_cast<double>(end - start) * 1000.0) / frequency;
}
std::uint64_t filetime_ticks(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}


struct AdapterSelection {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
};

AdapterSelection select_intel_adapter() {
    ComPtr<IDXGIFactory1> factory;
    check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check_hresult(result, "IDXGIFactory1::EnumAdapters1");
        DXGI_ADAPTER_DESC1 description{};
        check_hresult(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if (description.VendorId == kIntelVendorId &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            return {std::move(adapter), description};
        }
    }
    throw std::runtime_error("No hardware Intel adapter was found");
}

class D3D11Roundtrip {
  public:
    explicit D3D11Roundtrip(IDXGIAdapter1* adapter) {
        constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        check_hresult(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                        levels.data(), static_cast<UINT>(levels.size()),
                                        D3D11_SDK_VERSION, &device_, &selected, &context_),
                      "D3D11CreateDevice");

        D3D11_TEXTURE2D_DESC source_desc{};
        source_desc.Width = sparkface::fallback::kWidth;
        source_desc.Height = sparkface::fallback::kHeight;
        source_desc.MipLevels = 1;
        source_desc.ArraySize = 1;
        source_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        source_desc.SampleDesc.Count = 1;
        source_desc.Usage = D3D11_USAGE_DEFAULT;
        source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check_hresult(device_->CreateTexture2D(&source_desc, nullptr, &source_),
                      "Create source texture");

        D3D11_TEXTURE2D_DESC upload_desc = source_desc;
        upload_desc.Usage = D3D11_USAGE_DYNAMIC;
        upload_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        upload_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hresult(device_->CreateTexture2D(&upload_desc, nullptr, &upload_),
                      "Create upload texture");

        D3D11_TEXTURE2D_DESC readback_desc = source_desc;
        readback_desc.Usage = D3D11_USAGE_STAGING;
        readback_desc.BindFlags = 0;
        readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check_hresult(device_->CreateTexture2D(&readback_desc, nullptr, &readback_),
                      "Create readback texture");
    }

    void prepare_frame(std::uint64_t sequence) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context_->Map(upload_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map upload texture");
        for (std::uint32_t y = 0; y < sparkface::fallback::kHeight; ++y) {
            auto* row = static_cast<std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch;
            for (std::uint32_t x = 0; x < sparkface::fallback::kWidth; ++x) {
                const std::size_t offset = static_cast<std::size_t>(x) * 4;
                row[offset] = static_cast<std::uint8_t>((x + sequence) & 0xffU);
                row[offset + 1] = static_cast<std::uint8_t>((y + sequence) & 0xffU);
                row[offset + 2] = static_cast<std::uint8_t>((x + y + sequence) & 0xffU);
                row[offset + 3] = 0xffU;
            }
        }
        context_->Unmap(upload_.Get(), 0);
        context_->CopyResource(source_.Get(), upload_.Get());
    }

    double readback(std::byte* destination) {
        const std::int64_t start = qpc_now();
        context_->CopyResource(readback_.Get(), source_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                      "Map readback texture");
        constexpr std::size_t row_bytes =
            static_cast<std::size_t>(sparkface::fallback::kWidth) * 4;
        for (std::uint32_t y = 0; y < sparkface::fallback::kHeight; ++y) {
            const auto* source = static_cast<const std::byte*>(mapped.pData) +
                                 static_cast<std::size_t>(y) * mapped.RowPitch;
            std::memcpy(destination + static_cast<std::size_t>(y) * row_bytes,
                        source, row_bytes);
        }
        context_->Unmap(readback_.Get(), 0);
        return qpc_ms(start, qpc_now());
    }

  private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> source_;
    ComPtr<ID3D11Texture2D> upload_;
    ComPtr<ID3D11Texture2D> readback_;
};

std::filesystem::path executable_directory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

class BgfxCallback final : public bgfx::CallbackI {
  public:
    void fatal(const char* file, std::uint16_t line, bgfx::Fatal::Enum code,
               const char* message) override {
        std::fprintf(stderr, "bgfx_fatal=%s:%u code=%u %s\n", file,
                     static_cast<unsigned>(line), static_cast<unsigned>(code), message);
        std::fflush(stderr);
        if (code != bgfx::Fatal::DebugCheck) {
            std::abort();
        }
    }

    void traceVargs(const char*, std::uint16_t, const char*, std::va_list) override {}

    void profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerEnd() override {}
    std::uint32_t cacheReadSize(std::uint64_t) override { return 0; }
    bool cacheRead(std::uint64_t, void*, std::uint32_t) override { return false; }
    void cacheWrite(std::uint64_t, const void*, std::uint32_t) override {}
    void screenShot(const char*, std::uint32_t, std::uint32_t, std::uint32_t,
                    const void*, std::uint32_t, bool) override {}
    void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t,
                      bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, std::uint32_t) override {}
};

class BgfxRoundtrip {
  public:
    explicit BgfxRoundtrip(const DXGI_ADAPTER_DESC1& adapter) {
        window_ = CreateWindowExW(0, L"STATIC", L"SparkFace Phase-0 B2", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 16, 16, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (window_ == nullptr) {
            throw std::runtime_error("CreateWindowExW failed");
        }

        bgfx::Init init;
        init.type = bgfx::RendererType::Direct3D12;
        init.vendorId = static_cast<std::uint16_t>(adapter.VendorId);
        init.deviceId = static_cast<std::uint16_t>(adapter.DeviceId);
        init.platformData.nwh = window_;
        init.resolution.width = 16;
        init.resolution.height = 16;
        init.resolution.reset = BGFX_RESET_NONE;
        init.callback = &callback_;
        if (!bgfx::init(init)) {
            throw std::runtime_error("bgfx D3D12 initialization failed");
        }
        initialized_ = true;

        const bgfx::Caps* caps = bgfx::getCaps();
        if (caps == nullptr || caps->rendererType != bgfx::RendererType::Direct3D12 ||
            (caps->supported & BGFX_CAPS_COMPUTE) == 0 ||
            (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0) {
            throw std::runtime_error("bgfx D3D12 compute/readback capability is unavailable");
        }

        input_ = bgfx::createTexture2D(
            static_cast<std::uint16_t>(sparkface::fallback::kWidth),
            static_cast<std::uint16_t>(sparkface::fallback::kHeight), false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_COMPUTE_WRITE);
        output_ = bgfx::createTexture2D(
            static_cast<std::uint16_t>(sparkface::fallback::kWidth),
            static_cast<std::uint16_t>(sparkface::fallback::kHeight), false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_COMPUTE_WRITE);
        readback_ = bgfx::createTexture2D(
            static_cast<std::uint16_t>(sparkface::fallback::kWidth),
            static_cast<std::uint16_t>(sparkface::fallback::kHeight), false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        if (!bgfx::isValid(input_) || !bgfx::isValid(output_) || !bgfx::isValid(readback_)) {
            throw std::runtime_error("bgfx texture creation failed");
        }

        const auto shader_path = executable_directory() / L"cs_fallback.bin";
        std::ifstream shader_file(shader_path, std::ios::binary | std::ios::ate);
        if (!shader_file) {
            throw std::runtime_error("Failed to open cs_fallback.bin");
        }
        const auto shader_size = shader_file.tellg();
        if (shader_size <= 0 || shader_size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("Invalid cs_fallback.bin size");
        }
        shader_file.seekg(0);
        std::vector<std::byte> shader(static_cast<std::size_t>(shader_size));
        shader_file.read(reinterpret_cast<char*>(shader.data()), shader_size);
        if (!shader_file) {
            throw std::runtime_error("Failed to read cs_fallback.bin");
        }
        const bgfx::Memory* memory =
            bgfx::copy(shader.data(), static_cast<std::uint32_t>(shader.size()));
        program_ = bgfx::createProgram(bgfx::createShader(memory), true);
        if (!bgfx::isValid(program_)) {
            throw std::runtime_error("bgfx compute program creation failed");
        }
    }

    ~BgfxRoundtrip() {
        if (initialized_) {
            if (bgfx::isValid(program_)) {
                bgfx::destroy(program_);
            }
            if (bgfx::isValid(readback_)) {
                bgfx::destroy(readback_);
            }
            if (bgfx::isValid(output_)) {
                bgfx::destroy(output_);
            }
            if (bgfx::isValid(input_)) {
                bgfx::destroy(input_);
            }
            bgfx::shutdown();
        }
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    double run(const std::byte* input, std::byte* output) {
        constexpr std::uint32_t rgba_bytes =
            sparkface::fallback::kWidth * sparkface::fallback::kHeight * 4;
        const std::int64_t start = qpc_now();
        const bgfx::Memory* memory = bgfx::copy(input, rgba_bytes);
        bgfx::updateTexture2D(input_, 0, 0, 0, 0,
                              static_cast<std::uint16_t>(sparkface::fallback::kWidth),
                              static_cast<std::uint16_t>(sparkface::fallback::kHeight),
                              memory, static_cast<std::uint16_t>(sparkface::fallback::kWidth * 4));
        bgfx::setImage(0, input_, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA8);
        bgfx::setImage(1, output_, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA8);
        bgfx::dispatch(0, program_, (sparkface::fallback::kWidth + 15) / 16,
                       (sparkface::fallback::kHeight + 15) / 16, 1);
        bgfx::blit(1, readback_, 0, 0, output_);
        bgfx::frame();
        const std::uint32_t ready_frame = bgfx::readTexture(readback_, output);
        while (bgfx::frame() < ready_frame) {
        }
        return qpc_ms(start, qpc_now());
    }

  private:
    BgfxCallback callback_;
    HWND window_ = nullptr;
    bool initialized_ = false;
    bgfx::TextureHandle input_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle output_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readback_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
};

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void rgba_to_nv12(const std::byte* rgba, std::byte* nv12, std::uint64_t sequence,
                  std::int64_t producer_qpc) {
    auto* y_plane = reinterpret_cast<std::uint8_t*>(nv12);
    auto* uv_plane = y_plane + static_cast<std::size_t>(sparkface::fallback::kWidth) *
                                   sparkface::fallback::kHeight;
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(rgba);
    for (std::uint32_t y = 0; y < sparkface::fallback::kHeight; y += 2) {
        for (std::uint32_t x = 0; x < sparkface::fallback::kWidth; x += 2) {
            int u_sum = 0;
            int v_sum = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::size_t pixel =
                        (static_cast<std::size_t>(y + dy) * sparkface::fallback::kWidth + x + dx) * 4;
                    const int r = pixels[pixel];
                    const int g = pixels[pixel + 1];
                    const int b = pixels[pixel + 2];
                    const int luma = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    y_plane[static_cast<std::size_t>(y + dy) * sparkface::fallback::kWidth + x + dx] =
                        clamp_byte(luma);
                    u_sum += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    v_sum += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                }
            }
            const std::size_t uv = static_cast<std::size_t>(y / 2) *
                                       sparkface::fallback::kWidth + x;
            uv_plane[uv] = clamp_byte(u_sum / 4);
            uv_plane[uv + 1] = clamp_byte(v_sum / 4);
        }
    }

    std::memcpy(nv12, "SFB2", 4);
    std::memcpy(nv12 + 4, &sequence, sizeof(sequence));
    const std::uint32_t check =
        static_cast<std::uint32_t>(sequence ^ (sequence >> 32) ^ 0xB2F00D5AU);
    std::memcpy(nv12 + 12, &check, sizeof(check));
    std::memcpy(nv12 + 16, &producer_qpc, sizeof(producer_qpc));
}

void validate_compute_copy(const std::byte* expected, const std::byte* actual) {
    constexpr std::array<std::size_t, 8> pixels{
        0, 1, 1919, 1920, 1920 * 539 + 960, 1920 * 1079, 1920 * 1080 - 2,
        1920 * 1080 - 1};
    for (const std::size_t pixel : pixels) {
        const std::size_t offset = pixel * 4;
        if (std::memcmp(expected + offset, actual + offset, 4) != 0) {
            throw std::runtime_error("bgfx compute output validation failed");
        }
    }
}

struct FrameSample {
    double d3d11_ms;
    double bgfx_ms;
    double nv12_ms;
    double publish_ms;
    double total_ms;
};

struct Options {
    std::uint64_t frames = 1000;
    std::uint64_t soak_seconds = 0;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if ((argument == "--frames" || argument == "--soak-seconds") && i + 1 >= argc) {
            throw std::runtime_error(std::string(argument) + " requires a value");
        }
        if (argument == "--frames") {
            options.frames = std::stoull(argv[++i]);
        } else if (argument == "--soak-seconds") {
            options.soak_seconds = std::stoull(argv[++i]);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(argument));
        }
    }
    if (options.frames == 0 && options.soak_seconds == 0) {
        throw std::runtime_error("At least one frame is required");
    }
    if (options.soak_seconds > 0) {
        options.frames = options.soak_seconds * sparkface::fallback::kFrameRate;
    }
    return options;
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    return values[static_cast<std::size_t>(std::ceil(position))];
}

void report_metric(const char* name, const std::vector<double>& values) {
    std::cout << name << "_p50_ms=" << percentile(values, 0.50) << '\n'
              << name << "_p95_ms=" << percentile(values, 0.95) << '\n'
              << name << "_p99_ms=" << percentile(values, 0.99) << '\n'
              << name << "_max_ms=" << *std::max_element(values.begin(), values.end()) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const AdapterSelection adapter = select_intel_adapter();
        D3D11Roundtrip d3d11(adapter.adapter.Get());
        BgfxRoundtrip bgfx(adapter.description);
        sparkface::fallback::ProducerRing ring;

        constexpr std::size_t rgba_bytes =
            static_cast<std::size_t>(sparkface::fallback::kWidth) *
            sparkface::fallback::kHeight * 4;
        std::vector<std::byte> d3d11_frame(rgba_bytes);
        std::vector<std::byte> bgfx_frame(rgba_bytes);
        std::vector<std::byte> nv12_frame(sparkface::fallback::kNv12Bytes);

        std::uint64_t sequence = 1;
        for (; sequence <= kWarmupFrames; ++sequence) {
            d3d11.prepare_frame(sequence);
            d3d11.readback(d3d11_frame.data());
            bgfx.run(d3d11_frame.data(), bgfx_frame.data());
            validate_compute_copy(d3d11_frame.data(), bgfx_frame.data());
            const std::int64_t warmup_qpc = qpc_now();
            rgba_to_nv12(bgfx_frame.data(), nv12_frame.data(), sequence, warmup_qpc);
            ring.publish(nv12_frame.data(), sequence, qpc_now(), qpc_now());
        }

        FILETIME creation_time{};
        FILETIME exit_time{};
        FILETIME kernel_before{};
        FILETIME user_before{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                             &kernel_before, &user_before)) {
            throw std::runtime_error("GetProcessTimes failed before measurement");
        }
        const std::int64_t measurement_start = qpc_now();
        std::vector<FrameSample> samples;
        samples.reserve(static_cast<std::size_t>(options.frames));
        std::uint64_t missed_deadlines = 0;
        auto next_deadline = std::chrono::steady_clock::now();
        for (std::uint64_t frame = 0; frame < options.frames; ++frame, ++sequence) {
            next_deadline += std::chrono::nanoseconds(1'000'000'000 / sparkface::fallback::kFrameRate);
            d3d11.prepare_frame(sequence);
            const std::int64_t total_start = qpc_now();
            const double d3d11_ms = d3d11.readback(d3d11_frame.data());
            const double bgfx_ms = bgfx.run(d3d11_frame.data(), bgfx_frame.data());
            validate_compute_copy(d3d11_frame.data(), bgfx_frame.data());
            const std::int64_t nv12_start = qpc_now();
            rgba_to_nv12(bgfx_frame.data(), nv12_frame.data(), sequence, total_start);
            const double nv12_ms = qpc_ms(nv12_start, qpc_now());
            const std::int64_t publish_start = qpc_now();
            ring.publish(nv12_frame.data(), sequence, total_start, qpc_now());
            const double publish_ms = qpc_ms(publish_start, qpc_now());
            const double total_ms = qpc_ms(total_start, qpc_now());
            samples.push_back({d3d11_ms, bgfx_ms, nv12_ms, publish_ms, total_ms});
            if (total_ms > kFrameBudgetMs) {
                ++missed_deadlines;
            }
            std::this_thread::sleep_until(next_deadline);
        }
        const std::int64_t measurement_end = qpc_now();
        FILETIME kernel_after{};
        FILETIME user_after{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                             &kernel_after, &user_after)) {
            throw std::runtime_error("GetProcessTimes failed after measurement");
        }
        const double measurement_ms = qpc_ms(measurement_start, measurement_end);
        const double process_cpu_ms = static_cast<double>(
            filetime_ticks(kernel_after) - filetime_ticks(kernel_before) +
            filetime_ticks(user_after) - filetime_ticks(user_before)) / 10000.0;
        const double achieved_fps =
            static_cast<double>(options.frames) * 1000.0 / measurement_ms;
        const double process_cpu_percent = process_cpu_ms * 100.0 / measurement_ms;


        std::vector<double> d3d11_values;
        std::vector<double> bgfx_values;
        std::vector<double> nv12_values;
        std::vector<double> publish_values;
        std::vector<double> total_values;
        d3d11_values.reserve(samples.size());
        bgfx_values.reserve(samples.size());
        nv12_values.reserve(samples.size());
        publish_values.reserve(samples.size());
        total_values.reserve(samples.size());
        for (const FrameSample& sample : samples) {
            d3d11_values.push_back(sample.d3d11_ms);
            bgfx_values.push_back(sample.bgfx_ms);
            nv12_values.push_back(sample.nv12_ms);
            publish_values.push_back(sample.publish_ms);
            total_values.push_back(sample.total_ms);
        }

        std::wcout << L"adapter=" << adapter.description.Description << L'\n';
        std::cout << std::fixed << std::setprecision(3)
                  << "frames=" << options.frames << '\n'
                  << "resolution=1920x1080\n"
                  << "fps_target=30\n"
                  << "fps_achieved=" << achieved_fps << '\n'
                  << "process_cpu_ms=" << process_cpu_ms << '\n'
                  << "process_cpu_percent=" << process_cpu_percent << '\n';
        report_metric("d3d11_readback", d3d11_values);
        report_metric("bgfx_roundtrip", bgfx_values);
        report_metric("rgba_to_nv12", nv12_values);
        report_metric("ring_publish", publish_values);
        report_metric("transport_total", total_values);
        std::cout << "missed_33_3ms_deadlines=" << missed_deadlines << '\n'
                  << "content_validation=pass\n"
                  << "product_30fps_proven=no\n"
                  << "FALLBACK_PROBE_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "probe_error=" << error.what() << '\n';
        return 1;
    }
}
