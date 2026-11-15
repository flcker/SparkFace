// SPDX-License-Identifier: Apache-2.0
#pragma once

// This probe-only renderer runs inside the Media Foundation source process. It
// either renders the retained deterministic marker frame or captures a selected
// physical camera on the allocator D3D11 device, overlays the marker, and
// converts directly into the allocator-owned NV12 sample.

#include <Windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "PhysicalCameraCapture.h"
#include "control_protocol.h"
#pragma comment(lib, "d3d11")
#pragma comment(lib, "d3dcompiler")
#pragma comment(lib, "dxgi")

namespace SparkFaceSourceLocal {

inline constexpr wchar_t EnablePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportSourceLocal.flag";
inline constexpr wchar_t TracePath[] =
    L"C:\\Users\\Public\\SparkFaceSourceLocalTrace.log";
inline constexpr std::uint16_t PayloadMagic = 0xD311U;

inline void Trace(const char* message) {
    HANDLE file = CreateFileW(TracePath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &written,
              nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

class Renderer final {
  public:
    HRESULT Initialize() {
        enabled_ = GetFileAttributesW(EnablePath) != INVALID_FILE_ATTRIBUTES;
        h9Enabled_ = enabled_ &&
                     GetFileAttributesW(SparkFaceSourceLocalControl::EnablePath) !=
                         INVALID_FILE_ATTRIBUTES;
        if (enabled_) {
            Trace("source_local_enabled=1");
        }
        if (h9Enabled_) {
            assetStatus_ = ValidateAsset();
            if (assetStatus_ == SparkFaceSourceLocalControl::Status::Applied) {
                assetStatus_ = OpenControlMapping();
            }
            TraceH9Status(assetStatus_, 0, 0);
        }
        return S_OK;
    }

    ~Renderer() {
        if (controlView_ != nullptr) {
            UnmapViewOfFile(controlView_);
        }
        if (controlMapping_ != nullptr) {
            CloseHandle(controlMapping_);
        }
        if (producerProcess_ != nullptr) {
            CloseHandle(producerProcess_);
        }
    }

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::uint64_t conversion_count() const {
        return conversionCount_;
    }

    HRESULT RenderSample(IMFSample* sample, bool* delivered) {
        RETURN_HR_IF_NULL(E_POINTER, sample);
        RETURN_HR_IF_NULL(E_POINTER, delivered);
        *delivered = false;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
        RETURN_IF_FAILED(sample->GetBufferByIndex(0, &mediaBuffer));
        Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
        RETURN_IF_FAILED(mediaBuffer.As(&dxgiBuffer));
        Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
        RETURN_IF_FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&outputTexture)));
        UINT outputSubresource = 0;
        RETURN_IF_FAILED(dxgiBuffer->GetSubresourceIndex(&outputSubresource));

        D3D11_TEXTURE2D_DESC outputDescription{};
        outputTexture->GetDesc(&outputDescription);
        RETURN_HR_IF(MF_E_UNSUPPORTED_FORMAT,
                     outputDescription.Format != DXGI_FORMAT_NV12 ||
                         outputDescription.Width == 0 ||
                         outputDescription.Height == 0);

        LARGE_INTEGER now{};
        RETURN_LAST_ERROR_IF(!QueryPerformanceCounter(&now));
        const ControlState control = ReadControl(now.QuadPart);
        RETURN_IF_FAILED(EnsurePipeline(outputTexture.Get(), outputDescription));
        RETURN_IF_FAILED(PreparePhysicalCamera(control));

        std::uint64_t sequence = conversionCount_ + 1;
        std::int64_t markerQpc = now.QuadPart;
        if (control.physicalCamera) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> capturedTexture;
            UINT capturedSubresource = 0;
            const HRESULT captureResult = physicalCapture_.AcquireNext(
                &capturedTexture, &capturedSubresource, &sequence, &markerQpc);
            if (captureResult == E_PENDING) {
                return S_OK;
            }
            RETURN_IF_FAILED(captureResult);
            context_->CopySubresourceRegion(renderTexture_.Get(), 0, 0, 0, 0,
                                            capturedTexture.Get(),
                                            capturedSubresource, nullptr);
        }
        RETURN_IF_FAILED(RenderBgra(sequence, markerQpc, control));
        RETURN_IF_FAILED(ConvertToAllocatorTexture(
            outputTexture.Get(), outputDescription, outputSubresource));
        context_->Flush();

        conversionCount_ = sequence;
        if (conversionCount_ == 1 || (conversionCount_ % 100) == 0) {
            char message[96]{};
            sprintf_s(message, "source_local_gpu_conversion_count=%llu",
                      static_cast<unsigned long long>(conversionCount_));
            Trace(message);
        }
        *delivered = true;
        return S_OK;
    }

    struct MarkerConstants {
        std::uint32_t sequenceLow;
        std::uint32_t sequenceHigh;
        std::uint32_t qpcLow;
        std::uint32_t qpcHigh;
        std::uint32_t sourcePid;
        std::uint32_t frame;
        std::uint32_t controlRevision;
        std::uint32_t controlEffectValue;
        std::uint32_t controlStatus;
        std::uint32_t physicalCamera;
        std::array<std::uint32_t, 2> padding{};
    };
    static_assert(sizeof(MarkerConstants) % 16 == 0);

    struct ControlState {
        std::uint32_t revision = 0;
        std::uint32_t effectValue = 0;
        SparkFaceSourceLocalControl::Status status =
            SparkFaceSourceLocalControl::Status::Disabled;
        bool physicalCamera = false;
        std::uint32_t cameraSymbolicLinkLength = 0;
        std::array<wchar_t,
                   SparkFaceSourceLocalControl::CameraSymbolicLinkCapacity>
            cameraSymbolicLink{};
    };

    struct OutputViewEntry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    };

    static constexpr std::size_t OutputViewCapacity = 10;

    static constexpr char VertexShader[] = R"(
struct Output { float4 position : SV_Position; };
Output main(uint vertex_id : SV_VertexID) {
    float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    Output output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    return output;
}
)";

    static constexpr char PixelShader[] = R"(
#define MARKER_PIXEL_WIDTH 6u
cbuffer Marker : register(b0) {
    uint sequence_low;
    uint sequence_high;
    uint qpc_low;
    uint qpc_high;
    uint source_pid;
    uint frame;
    uint control_revision;
    uint control_effect_value;
    uint control_status;
    uint physical_camera;
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
    if (index < 32) return (source_pid >> index) & 1u;
    index -= 32;
    if (index < 32) return (control_revision >> index) & 1u;
    index -= 32;
    if (index < 32) return (control_effect_value >> index) & 1u;
    index -= 32;
    return (control_status >> index) & 1u;
}
float4 main(float4 position : SV_Position) : SV_Target {
    uint bit = (uint)position.x / MARKER_PIXEL_WIDTH;
    if (position.y < 8.0 && bit < 272u) {
        float value = marker_bit(bit) != 0u ? 1.0 : 0.0;
        return float4(value, value, value, 1.0);
    }
    if (physical_camera != 0u) discard;
    float2 uv = position.xy / float2(1920.0, 1080.0);
    float phase = (float)(frame % 300u) / 300.0;
    float effect = (float)control_effect_value / 100.0;
    return float4(frac(uv.x + phase + effect), frac(uv.y + phase * 0.5),
                  frac(uv.x + uv.y + phase + effect * 0.25), 1.0);
}
)";

    static constexpr char ModuleAnchor = 0;

    SparkFaceSourceLocalControl::Status ValidateAsset() {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleAnchor),
                                &module)) {
            return SparkFaceSourceLocalControl::Status::AssetMissing;
        }
        std::array<wchar_t, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameW(module, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return SparkFaceSourceLocalControl::Status::AssetMissing;
        }
        wchar_t* separator = wcsrchr(path.data(), L'\\');
        if (separator == nullptr ||
            static_cast<std::size_t>(path.data() + path.size() - separator) <=
                wcslen(SparkFaceSourceLocalControl::AssetName)) {
            return SparkFaceSourceLocalControl::Status::AssetMissing;
        }
        wcscpy_s(separator + 1, path.data() + path.size() - separator - 1,
                 SparkFaceSourceLocalControl::AssetName);
        HANDLE file = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return SparkFaceSourceLocalControl::Status::AssetMissing;
        }
        LARGE_INTEGER size{};
        std::array<char, SparkFaceSourceLocalControl::AssetByteCount> bytes{};
        DWORD read = 0;
        const bool valid = GetFileSizeEx(file, &size) &&
                           size.QuadPart == static_cast<LONGLONG>(bytes.size()) &&
                           ReadFile(file, bytes.data(),
                                    static_cast<DWORD>(bytes.size()), &read,
                                    nullptr) &&
                           read == bytes.size() &&
                           SparkFaceSourceLocalControl::AssetHashBytes(
                               bytes.data(), bytes.size()) ==
                               SparkFaceSourceLocalControl::AssetHash;
        CloseHandle(file);
        return valid ? SparkFaceSourceLocalControl::Status::Applied
                     : SparkFaceSourceLocalControl::Status::AssetInvalid;
    }

    SparkFaceSourceLocalControl::Status OpenControlMapping() {
        controlMapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                           SparkFaceSourceLocalControl::MappingName);
        if (controlMapping_ == nullptr) {
            return SparkFaceSourceLocalControl::Status::MappingUnavailable;
        }
        controlView_ = static_cast<SparkFaceSourceLocalControl::ControlBlock*>(
            MapViewOfFile(controlMapping_, FILE_MAP_READ, 0, 0,
                          sizeof(SparkFaceSourceLocalControl::ControlBlock)));
        if (controlView_ == nullptr) {
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return SparkFaceSourceLocalControl::Status::MappingUnavailable;
        }
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            UnmapViewOfFile(controlView_);
            controlView_ = nullptr;
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return SparkFaceSourceLocalControl::Status::MappingUnavailable;
        }
        qpcFrequency_ = frequency.QuadPart;
        return SparkFaceSourceLocalControl::Status::Applied;
    }

    void TraceH9Status(SparkFaceSourceLocalControl::Status status,
                       std::uint32_t revision, std::uint32_t value) {
        if (lastTraceStatus_ == static_cast<std::uint32_t>(status) &&
            lastTraceRevision_ == revision && lastTraceValue_ == value) {
            return;
        }
        char message[128]{};
        sprintf_s(message, "source_local_h9_status=%u revision=%u value=%u",
                  static_cast<unsigned>(status), revision, value);
        Trace(message);
        lastTraceStatus_ = static_cast<std::uint32_t>(status);
        lastTraceRevision_ = revision;
        lastTraceValue_ = value;
    }

    ControlState ReadControl(std::int64_t nowQpc) {
        ControlState state{};
        if (!h9Enabled_) {
            return state;
        }
        if (assetStatus_ != SparkFaceSourceLocalControl::Status::Applied) {
            state.status = assetStatus_;
            TraceH9Status(state.status, 0, 0);
            return state;
        }
        if (controlView_ == nullptr) {
            state.status = SparkFaceSourceLocalControl::Status::MappingUnavailable;
            TraceH9Status(state.status, 0, 0);
            return state;
        }
        SparkFaceSourceLocalControl::Snapshot snapshot{};
        bool snapshotRead = false;
        for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
            if (SparkFaceSourceLocalControl::ReadSnapshot(controlView_,
                                                          &snapshot)) {
                snapshotRead = true;
                break;
            }
            YieldProcessor();
        }
        if (snapshotRead) {
            lastControlSnapshot_ = snapshot;
            hasLastControlSnapshot_ = true;
        } else {
            const LONG sequence = ReadAcquire(&controlView_->publicationSequence);
            if ((sequence & 1) != 0 && hasLastControlSnapshot_) {
                snapshot = lastControlSnapshot_;
            } else {
                state.status = (sequence & 1) != 0
                                   ? SparkFaceSourceLocalControl::Status::PublicationBusy
                                   : SparkFaceSourceLocalControl::Status::ProtocolInvalid;
                TraceH9Status(state.status, 0, 0);
                return state;
            }
        }
        if (snapshot.revision == 0) {
            state.status = SparkFaceSourceLocalControl::Status::ProtocolInvalid;
            TraceH9Status(state.status, 0, 0);
            return state;
        }

        if (snapshot.producerPid == 0) {
            state.status = SparkFaceSourceLocalControl::Status::ProducerDead;
            TraceH9Status(state.status, 0, 0);
            return state;
        }
        if (snapshot.producerPid != producerPid_) {
            if (producerProcess_ != nullptr) {
                CloseHandle(producerProcess_);
                producerProcess_ = nullptr;
            }
            producerProcess_ = OpenProcess(SYNCHRONIZE |
                                               PROCESS_QUERY_LIMITED_INFORMATION,
                                           FALSE, snapshot.producerPid);
            producerPid_ = snapshot.producerPid;
        }
        if (producerProcess_ == nullptr ||
            WaitForSingleObject(producerProcess_, 0) != WAIT_TIMEOUT) {
            state.status = SparkFaceSourceLocalControl::Status::ProducerDead;
            TraceH9Status(state.status, 0, 0);
            return state;
        }
        if (!SparkFaceSourceLocalControl::IsHeartbeatFresh(
                snapshot.heartbeatQpc, nowQpc, qpcFrequency_)) {
            state.status = SparkFaceSourceLocalControl::Status::HeartbeatStale;
            TraceH9Status(state.status, 0, 0);
            return state;
        }
        if (!SparkFaceSourceLocalControl::IsEffectValueValid(snapshot.effectValue)) {
            state.status = SparkFaceSourceLocalControl::Status::ValueInvalid;
            TraceH9Status(state.status, 0, 0);
            return state;
        }
        state.revision = snapshot.revision;
        state.effectValue = snapshot.effectValue;
        state.status = SparkFaceSourceLocalControl::Status::Applied;
        state.physicalCamera =
            SparkFaceSourceLocalControl::IsPhysicalCameraEnabled(snapshot);
        state.cameraSymbolicLinkLength = snapshot.cameraSymbolicLinkLength;
        state.cameraSymbolicLink = snapshot.cameraSymbolicLink;
        TraceH9Status(state.status, state.revision, state.effectValue);
        return state;
    }

    static std::uint32_t CameraSelectionHash(const ControlState& control) {
        std::uint32_t hash = 2166136261U;
        for (std::uint32_t index = 0;
             index < control.cameraSymbolicLinkLength; ++index) {
            const std::uint16_t value =
                static_cast<std::uint16_t>(control.cameraSymbolicLink[index]);
            hash ^= value & 0xFFU;
            hash *= 16777619U;
            hash ^= value >> 8;
            hash *= 16777619U;
        }
        return hash;
    }

    HRESULT PreparePhysicalCamera(const ControlState& control) {
        if (!control.physicalCamera ||
            control.status != SparkFaceSourceLocalControl::Status::Applied) {
            if (physicalCameraInitialized_) {
                physicalCapture_.Shutdown();
                physicalCameraInitialized_ = false;
                physicalCameraSelectionHash_ = 0;
            }
            return S_OK;
        }
        const std::uint32_t selectionHash = CameraSelectionHash(control);
        if (physicalCameraInitialized_ &&
            selectionHash == physicalCameraSelectionHash_) {
            return S_OK;
        }
        physicalCapture_.Shutdown();
        physicalCameraInitialized_ = false;
        RETURN_IF_FAILED(physicalCapture_.Initialize(
            device_.Get(), control.cameraSymbolicLink.data(),
            control.cameraSymbolicLinkLength, width_, height_, 30));
        physicalCameraInitialized_ = true;
        physicalCameraSelectionHash_ = selectionHash;
        const LUID luid = physicalCapture_.allocator_luid();
        char message[160]{};
        sprintf_s(message,
                  "physical_camera_protocol=2 symbolic_link_hash=%08x adapter_luid=%08lx:%08lx",
                  selectionHash, static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
        Trace(message);
        return S_OK;
    }

    HRESULT CompileShader(const char* source, const char* target,
                          ID3DBlob** bytecode) {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source, std::strlen(source), nullptr, nullptr, nullptr, "main",
            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode, &errors);
        if (FAILED(result) && errors) {
            Trace(static_cast<const char*>(errors->GetBufferPointer()));
        }
        return result;
    }

    HRESULT EnsurePipeline(ID3D11Texture2D* outputTexture,
                           const D3D11_TEXTURE2D_DESC& outputDescription) {
        Microsoft::WRL::ComPtr<ID3D11Device> targetDevice;
        outputTexture->GetDevice(&targetDevice);
        RETURN_HR_IF_NULL(E_UNEXPECTED, targetDevice.Get());
        if (device_.Get() == targetDevice.Get() &&
            width_ == outputDescription.Width &&
            height_ == outputDescription.Height) {
            return S_OK;
        }

        ResetPipeline();
        device_ = targetDevice;
        width_ = outputDescription.Width;
        height_ = outputDescription.Height;
        device_->GetImmediateContext(&context_);
        RETURN_HR_IF_NULL(E_UNEXPECTED, context_.Get());
        RETURN_IF_FAILED(device_.As(&videoDevice_));
        RETURN_IF_FAILED(context_.As(&videoContext_));

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        RETURN_IF_FAILED(device_.As(&dxgiDevice));
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        RETURN_IF_FAILED(dxgiDevice->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC adapterDescription{};
        RETURN_IF_FAILED(adapter->GetDesc(&adapterDescription));
        char adapterMessage[128]{};
        sprintf_s(adapterMessage,
                  "source_local_adapter_luid=%08lx:%08lx",
                  static_cast<unsigned long>(adapterDescription.AdapterLuid.HighPart),
                  static_cast<unsigned long>(adapterDescription.AdapterLuid.LowPart));
        Trace(adapterMessage);

        Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
        RETURN_IF_FAILED(CompileShader(VertexShader, "vs_5_0", &vertexBytecode));
        RETURN_IF_FAILED(CompileShader(PixelShader, "ps_5_0", &pixelBytecode));
        RETURN_IF_FAILED(device_->CreateVertexShader(
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
            nullptr, &vertexShader_));
        RETURN_IF_FAILED(device_->CreatePixelShader(
            pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
            nullptr, &pixelShader_));

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(MarkerConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        RETURN_IF_FAILED(device_->CreateBuffer(&constantDescription, nullptr,
                                               &constantBuffer_));

        D3D11_TEXTURE2D_DESC renderDescription{};
        renderDescription.Width = width_;
        renderDescription.Height = height_;
        renderDescription.MipLevels = 1;
        renderDescription.ArraySize = 1;
        renderDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        renderDescription.SampleDesc.Count = 1;
        renderDescription.Usage = D3D11_USAGE_DEFAULT;
        renderDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        RETURN_IF_FAILED(device_->CreateTexture2D(
            &renderDescription, nullptr, &renderTexture_));
        RETURN_IF_FAILED(device_->CreateRenderTargetView(
            renderTexture_.Get(), nullptr, &renderTarget_));

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDescription{};
        contentDescription.InputFrameFormat =
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDescription.InputWidth = width_;
        contentDescription.InputHeight = height_;
        contentDescription.OutputWidth = width_;
        contentDescription.OutputHeight = height_;
        contentDescription.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        contentDescription.InputFrameRate.Numerator = 30;
        contentDescription.InputFrameRate.Denominator = 1;
        contentDescription.OutputFrameRate = contentDescription.InputFrameRate;
        RETURN_IF_FAILED(videoDevice_->CreateVideoProcessorEnumerator(
            &contentDescription, &videoEnumerator_));
        UINT inputFlags = 0;
        UINT outputFlags = 0;
        RETURN_IF_FAILED(videoEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &inputFlags));
        RETURN_IF_FAILED(videoEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_NV12, &outputFlags));
        RETURN_HR_IF(MF_E_UNSUPPORTED_FORMAT,
                     (inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
                         (outputFlags &
                          D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0);
        RETURN_IF_FAILED(videoDevice_->CreateVideoProcessor(
            videoEnumerator_.Get(), 0, &videoProcessor_));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDescription{};
        inputViewDescription.FourCC = 0;
        inputViewDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDescription.Texture2D.MipSlice = 0;
        inputViewDescription.Texture2D.ArraySlice = 0;
        RETURN_IF_FAILED(videoDevice_->CreateVideoProcessorInputView(
            renderTexture_.Get(), videoEnumerator_.Get(),
            &inputViewDescription, &videoInputView_));
        return S_OK;
    }

    HRESULT RenderBgra(std::uint64_t sequence, std::int64_t qpc,
                       const ControlState& control) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        RETURN_IF_FAILED(context_->Map(constantBuffer_.Get(), 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        auto* constants = static_cast<MarkerConstants*>(mapped.pData);
        constants->sequenceLow = static_cast<std::uint32_t>(sequence);
        constants->sequenceHigh = static_cast<std::uint32_t>(sequence >> 32);
        constants->qpcLow = static_cast<std::uint32_t>(qpc);
        constants->qpcHigh = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(qpc) >> 32);
        constants->sourcePid = GetCurrentProcessId();
        constants->frame = static_cast<std::uint32_t>(sequence);
        constants->controlRevision = control.revision;
        constants->controlEffectValue = control.effectValue;
        constants->controlStatus = static_cast<std::uint32_t>(control.status);
        constants->physicalCamera = control.physicalCamera ? 1U : 0U;
        context_->Unmap(constantBuffer_.Get(), 0);

        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width_),
            static_cast<float>(height_), 0.0F, 1.0F};
        ID3D11RenderTargetView* target = renderTarget_.Get();
        ID3D11Buffer* constantsBuffer = constantBuffer_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, &constantsBuffer);
        context_->Draw(3, 0);
        return S_OK;
    }

    HRESULT GetOutputView(
        ID3D11Texture2D* outputTexture,
        const D3D11_TEXTURE2D_DESC& outputDescription,
        UINT outputSubresource,
        ID3D11VideoProcessorOutputView** outputView) {
        RETURN_HR_IF_NULL(E_POINTER, outputView);
        *outputView = nullptr;
        for (std::size_t index = 0; index < outputViewCount_; ++index) {
            const auto& entry = outputViews_[index];
            if (entry.texture.Get() == outputTexture &&
                entry.subresource == outputSubresource) {
                return entry.view.CopyTo(outputView);
            }
        }
        RETURN_HR_IF(MF_E_SAMPLEALLOCATOR_EMPTY,
                     outputViewCount_ == outputViews_.size());

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description{};
        if (outputDescription.ArraySize == 1) {
            description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            description.Texture2D.MipSlice = 0;
        } else {
            description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray.MipSlice =
                outputSubresource % outputDescription.MipLevels;
            description.Texture2DArray.FirstArraySlice =
                outputSubresource / outputDescription.MipLevels;
            description.Texture2DArray.ArraySize = 1;
        }
        auto& entry = outputViews_[outputViewCount_];
        RETURN_IF_FAILED(videoDevice_->CreateVideoProcessorOutputView(
            outputTexture, videoEnumerator_.Get(), &description, &entry.view));
        entry.texture = outputTexture;
        entry.subresource = outputSubresource;
        ++outputViewCount_;
        return entry.view.CopyTo(outputView);
    }

    HRESULT ConvertToAllocatorTexture(
        ID3D11Texture2D* outputTexture,
        const D3D11_TEXTURE2D_DESC& outputDescription,
        UINT outputSubresource) {
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        RETURN_IF_FAILED(GetOutputView(outputTexture, outputDescription,
                                       outputSubresource, &outputView));

        const RECT frame{0, 0, static_cast<LONG>(width_),
                         static_cast<LONG>(height_)};
        videoContext_->VideoProcessorSetOutputTargetRect(
            videoProcessor_.Get(), TRUE, &frame);
        videoContext_->VideoProcessorSetStreamSourceRect(
            videoProcessor_.Get(), 0, TRUE, &frame);
        videoContext_->VideoProcessorSetStreamDestRect(
            videoProcessor_.Get(), 0, TRUE, &frame);
        videoContext_->VideoProcessorSetStreamFrameFormat(
            videoProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = videoInputView_.Get();
        return videoContext_->VideoProcessorBlt(
            videoProcessor_.Get(), outputView.Get(), 0, 1, &stream);
    }

    void ResetPipeline() {
        physicalCapture_.Shutdown();
        physicalCameraInitialized_ = false;
        physicalCameraSelectionHash_ = 0;
        for (auto& entry : outputViews_) {
            entry.view.Reset();
            entry.texture.Reset();
            entry.subresource = 0;
        }
        outputViewCount_ = 0;
        videoInputView_.Reset();
        videoProcessor_.Reset();
        videoEnumerator_.Reset();
        renderTarget_.Reset();
        renderTexture_.Reset();
        constantBuffer_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
        videoContext_.Reset();
        videoDevice_.Reset();
        context_.Reset();
        device_.Reset();
        width_ = 0;
        height_ = 0;
    }

    std::array<OutputViewEntry, OutputViewCapacity> outputViews_{};
    std::size_t outputViewCount_ = 0;
    bool enabled_ = false;
    bool h9Enabled_ = false;
    SparkFaceSourceLocalControl::Status assetStatus_ =
        SparkFaceSourceLocalControl::Status::Disabled;
    HANDLE controlMapping_ = nullptr;
    SparkFaceSourceLocalControl::ControlBlock* controlView_ = nullptr;
    HANDLE producerProcess_ = nullptr;
    std::uint32_t producerPid_ = 0;
    std::int64_t qpcFrequency_ = 0;
    std::uint32_t lastTraceStatus_ = static_cast<std::uint32_t>(-1);
    std::uint32_t lastTraceRevision_ = static_cast<std::uint32_t>(-1);
    std::uint32_t lastTraceValue_ = static_cast<std::uint32_t>(-1);
    std::uint64_t conversionCount_ = 0;
    SparkFaceSourceLocalControl::Snapshot lastControlSnapshot_{};
    bool hasLastControlSnapshot_ = false;
    UINT width_ = 0;
    UINT height_ = 0;
    bool physicalCameraInitialized_ = false;
    std::uint32_t physicalCameraSelectionHash_ = 0;
    PhysicalCameraCapture physicalCapture_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> renderTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> videoInputView_;
};

} // namespace SparkFaceSourceLocal
