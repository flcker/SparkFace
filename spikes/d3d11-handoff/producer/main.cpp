// SPDX-License-Identifier: Apache-2.0
#include "frame_transport.h"

// clang-format off
#include <Windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
// clang-format on

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;
using namespace sparkface::d3d11_handoff;

namespace {

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed: 0x" +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
}
void acquire_mutex(IDXGIKeyedMutex* mutex, UINT64 key, const char* operation) {
    const HRESULT result = mutex->AcquireSync(key, INFINITE);
    if (result != S_OK) {
        throw std::runtime_error(std::string(operation) + " failed: 0x" +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
}


std::int64_t qpc_now() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) {
        throw std::runtime_error("QueryPerformanceCounter failed");
    }
    return value.QuadPart;
}

struct Options {
    std::uint64_t frames = 1200;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--frames" && index + 1 < argc) {
            options.frames = std::stoull(argv[++index]);
            if (options.frames == 0) {
                throw std::invalid_argument("--frames must be positive");
            }
        } else {
            throw std::invalid_argument("usage: d3d11_handoff_producer [--frames N]");
        }
    }
    return options;
}

struct MarkerConstants {
    std::uint32_t sequence_low;
    std::uint32_t sequence_high;
    std::uint32_t qpc_low;
    std::uint32_t qpc_high;
    std::uint32_t producer_pid;
    std::uint32_t frame;
    std::array<std::uint32_t, 2> padding{};
};
static_assert(sizeof(MarkerConstants) % 16 == 0);

constexpr char kVertexShader[] = R"(
struct Output { float4 position : SV_Position; };
Output main(uint vertex_id : SV_VertexID) {
    float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    Output output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    return output;
}
)";

constexpr char kPixelShader[] = R"(
cbuffer Marker : register(b0) {
    uint sequence_low;
    uint sequence_high;
    uint qpc_low;
    uint qpc_high;
    uint producer_pid;
    uint frame;
    uint2 padding;
};

uint marker_bit(uint index) {
    if (index < 16) return (0xD311u >> index) & 1u;
    index -= 16;
    if (index < 32) return (sequence_low >> index) & 1u;
    index -= 32;
    if (index < 32) return (sequence_high >> index) & 1u;
    index -= 32;
    if (index < 32) return (qpc_low >> index) & 1u;
    index -= 32;
    if (index < 32) return (qpc_high >> index) & 1u;
    index -= 32;
    return (producer_pid >> index) & 1u;
}

float4 main(float4 position : SV_Position) : SV_Target {
    uint block = (uint)(position.x / 8.0);
    if (position.y < 8.0 && block < 176u) {
        float value = marker_bit(block) != 0u ? 1.0 : 0.0;
        return float4(value, value, value, 1.0);
    }
    float2 uv = position.xy / float2(1920.0, 1080.0);
    float phase = (float)(frame % 300u) / 300.0;
    return float4(frac(uv.x + phase), frac(uv.y + phase * 0.5), frac(uv.x + uv.y + phase), 1.0);
}
)";

ComPtr<ID3DBlob> compile_shader(const char* source, const char* target) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main",
                                      target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode,
                                      &errors);
    if (FAILED(result)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer())
                                     : "unknown shader error";
        throw std::runtime_error(message);
    }
    return bytecode;
}

struct SharedSlot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView1> luma_view;
    ComPtr<ID3D11RenderTargetView1> chroma_view;
    ComPtr<IDXGIKeyedMutex> mutex;
    Handle shared_handle;
};

class Producer {
  public:
    Producer() {
        create_device();
        create_render_pipeline();
        create_shared_slots();
    }

    void run(std::uint64_t frame_count) {
        ProducerControl control(adapter_luid_);
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency)) {
            throw std::runtime_error("QueryPerformanceFrequency failed");
        }
        const std::int64_t start = qpc_now();
        SharedSlot* outstanding = nullptr;
        for (std::uint64_t sequence = 1; sequence <= frame_count; ++sequence) {
            if (outstanding != nullptr) {
                acquire_mutex(outstanding->mutex.Get(), 0,
                              "IDXGIKeyedMutex::AcquireSync consumed frame");
                check_hresult(outstanding->mutex->ReleaseSync(0),
                              "IDXGIKeyedMutex::ReleaseSync consumed frame");
                outstanding = nullptr;
            }
            const std::int64_t target =
                start + static_cast<std::int64_t>((sequence - 1) * frequency.QuadPart / kFrameRate);
            while (true) {
                const std::int64_t remaining = target - qpc_now();
                if (remaining <= 0) {
                    break;
                }
                const DWORD sleep_ms = static_cast<DWORD>(remaining * 1000 / frequency.QuadPart);
                if (sleep_ms > 1) {
                    Sleep(sleep_ms - 1);
                } else {
                    SwitchToThread();
                }
            }

            const std::int64_t producer_qpc = qpc_now();
            const std::uint32_t slot_index = static_cast<std::uint32_t>(sequence % kSlotCount);
            SharedSlot& slot = slots_.at(slot_index);
            acquire_mutex(slot.mutex.Get(), 0, "IDXGIKeyedMutex::AcquireSync producer");
            render_pattern(slot, sequence, producer_qpc);
            context_->Flush();
            check_hresult(slot.mutex->ReleaseSync(1), "IDXGIKeyedMutex::ReleaseSync producer");
            const std::int64_t published_qpc = qpc_now();
            control.publish(slot_index, sequence, producer_qpc, published_qpc);
            outstanding = &slot;
        }
        const double elapsed_seconds =
            static_cast<double>(qpc_now() - start) / static_cast<double>(frequency.QuadPart);
        std::cout << std::fixed << std::setprecision(3)
                  << "producer_frames=" << frame_count << '\n'
                  << "producer_fps=" << static_cast<double>(frame_count - 1) / elapsed_seconds
                  << '\n'
                  << "full_frame_cpu_copy=0\n"
                  << "producer_path=D3D11_shader_to_shared_NV12_planes\n"
                  << "D3D11_HANDOFF_PRODUCER_PASS\n";
    }

  private:
    void create_device() {
        ComPtr<IDXGIFactory1> factory;
        check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> fallback;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            candidate->GetDesc1(&description);
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (!fallback) {
                fallback = candidate;
            }
            if (description.VendorId == 0x8086) {
                adapter_ = candidate;
                break;
            }
        }
        if (!adapter_) {
            adapter_ = fallback;
        }
        if (!adapter_) {
            throw std::runtime_error("No hardware DXGI adapter found");
        }
        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL feature_level{};
        check_hresult(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                        nullptr, 0, D3D11_SDK_VERSION, &device_, &feature_level,
                                        &context_),
                      "D3D11CreateDevice");
        DXGI_ADAPTER_DESC1 description{};
        adapter_->GetDesc1(&description);
        adapter_luid_ = description.AdapterLuid;
        std::wcout << L"adapter=" << description.Description << L'\n'
                   << L"adapter_luid=" << std::hex
                   << static_cast<unsigned long>(description.AdapterLuid.HighPart) << L":"
                   << static_cast<unsigned long>(description.AdapterLuid.LowPart)
                   << std::dec << L'\n';
    }

    void create_render_pipeline() {

        const ComPtr<ID3DBlob> vertex_bytecode = compile_shader(kVertexShader, "vs_5_0");
        const ComPtr<ID3DBlob> pixel_bytecode = compile_shader(kPixelShader, "ps_5_0");
        check_hresult(device_->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                                   vertex_bytecode->GetBufferSize(), nullptr,
                                                   &vertex_shader_),
                      "CreateVertexShader");
        check_hresult(device_->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
                                                  pixel_bytecode->GetBufferSize(), nullptr,
                                                  &pixel_shader_),
                      "CreatePixelShader");
        D3D11_BUFFER_DESC buffer_desc{};
        buffer_desc.ByteWidth = sizeof(MarkerConstants);
        buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hresult(device_->CreateBuffer(&buffer_desc, nullptr, &constant_buffer_),
                      "CreateBuffer marker");
    }


    void create_shared_slots() {
        check_hresult(device_.As(&device3_), "ID3D11Device3");
        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = kWidth;
        texture_desc.Height = kHeight;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_NV12;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture_desc.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        for (std::uint32_t index = 0; index < kSlotCount; ++index) {
            SharedSlot& slot = slots_.at(index);
            check_hresult(device_->CreateTexture2D(&texture_desc, nullptr, &slot.texture),
                          "CreateTexture2D shared NV12");
            check_hresult(slot.texture.As(&slot.mutex), "IDXGIKeyedMutex");
            D3D11_RENDER_TARGET_VIEW_DESC1 luma_desc{};
            luma_desc.Format = DXGI_FORMAT_R8_UNORM;
            luma_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            luma_desc.Texture2D.MipSlice = 0;
            luma_desc.Texture2D.PlaneSlice = 0;
            check_hresult(device3_->CreateRenderTargetView1(
                              slot.texture.Get(), &luma_desc, &slot.luma_view),
                          "CreateRenderTargetView1 luma");
            D3D11_RENDER_TARGET_VIEW_DESC1 chroma_desc = luma_desc;
            chroma_desc.Format = DXGI_FORMAT_R8G8_UNORM;
            chroma_desc.Texture2D.PlaneSlice = 1;
            check_hresult(device3_->CreateRenderTargetView1(
                              slot.texture.Get(), &chroma_desc, &slot.chroma_view),
                          "CreateRenderTargetView1 chroma");
            ComPtr<IDXGIResource1> resource;
            check_hresult(slot.texture.As(&resource), "IDXGIResource1");
            HANDLE shared_handle = nullptr;
            const std::wstring name = texture_name(GetCurrentProcessId(), index);
            check_hresult(resource->CreateSharedHandle(
                              permissive_security_attributes(),
                              DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                              name.c_str(), &shared_handle),
                          "CreateSharedHandle");
            slot.shared_handle = Handle(shared_handle);
        }
    }

    void render_pattern(SharedSlot& slot, std::uint64_t sequence,
                        std::int64_t producer_qpc) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context_->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map marker constant buffer");
        auto* constants = static_cast<MarkerConstants*>(mapped.pData);
        constants->sequence_low = static_cast<std::uint32_t>(sequence);
        constants->sequence_high = static_cast<std::uint32_t>(sequence >> 32);
        constants->qpc_low = static_cast<std::uint32_t>(producer_qpc);
        constants->qpc_high = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(producer_qpc) >> 32);
        constants->producer_pid = GetCurrentProcessId();
        constants->frame = static_cast<std::uint32_t>(sequence);
        context_->Unmap(constant_buffer_.Get(), 0);

        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(kWidth),
                                      static_cast<float>(kHeight), 0.0F, 1.0F};
        ID3D11RenderTargetView* render_target = slot.luma_view.Get();
        ID3D11Buffer* constant_buffer = constant_buffer_.Get();
        context_->OMSetRenderTargets(1, &render_target, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, &constant_buffer);
        context_->Draw(3, 0);
        const float neutral_chroma[4] = {128.0F / 255.0F, 128.0F / 255.0F, 0.0F, 1.0F};
        context_->ClearRenderTargetView(slot.chroma_view.Get(), neutral_chroma);
    }

    LUID adapter_luid_{};
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Device3> device3_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    std::array<SharedSlot, kSlotCount> slots_;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parse_options(argc, argv);
        Producer producer;
        producer.run(options.frames);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "D3D11_HANDOFF_PRODUCER_ERROR: " << error.what() << '\n';
        return 1;
    }
}
