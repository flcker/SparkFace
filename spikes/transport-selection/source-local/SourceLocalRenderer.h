// SPDX-License-Identifier: Apache-2.0
#pragma once

// This probe-only renderer runs inside the Media Foundation source process. It
// renders a deterministic BGRA marker frame on the allocator's D3D11 device and
// converts it directly into the allocator-owned NV12 sample.

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
        if (enabled_) {
            Trace("source_local_enabled=1");
        }
        return S_OK;
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
        RETURN_IF_FAILED(dxgiBuffer->GetResource(
            IID_PPV_ARGS(&outputTexture)));
        UINT outputSubresource = 0;
        RETURN_IF_FAILED(dxgiBuffer->GetSubresourceIndex(&outputSubresource));

        D3D11_TEXTURE2D_DESC outputDescription{};
        outputTexture->GetDesc(&outputDescription);
        RETURN_HR_IF(MF_E_UNSUPPORTED_FORMAT,
                     outputDescription.Format != DXGI_FORMAT_NV12 ||
                         outputDescription.Width == 0 ||
                         outputDescription.Height == 0);
        RETURN_IF_FAILED(EnsurePipeline(outputTexture.Get(), outputDescription));

        LARGE_INTEGER qpc{};
        RETURN_LAST_ERROR_IF(!QueryPerformanceCounter(&qpc));
        const std::uint64_t sequence = conversionCount_ + 1;
        RETURN_IF_FAILED(RenderBgra(sequence, qpc.QuadPart));
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

  private:
    struct MarkerConstants {
        std::uint32_t sequenceLow;
        std::uint32_t sequenceHigh;
        std::uint32_t qpcLow;
        std::uint32_t qpcHigh;
        std::uint32_t sourcePid;
        std::uint32_t frame;
        std::array<std::uint32_t, 2> padding{};
    };
    static_assert(sizeof(MarkerConstants) % 16 == 0);

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
cbuffer Marker : register(b0) {
    uint sequence_low;
    uint sequence_high;
    uint qpc_low;
    uint qpc_high;
    uint source_pid;
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
    return (source_pid >> index) & 1u;
}
float4 main(float4 position : SV_Position) : SV_Target {
    uint block = (uint)(position.x / 8.0);
    if (position.y < 8.0 && block < 176u) {
        float value = marker_bit(block) != 0u ? 1.0 : 0.0;
        return float4(value, value, value, 1.0);
    }
    float2 uv = position.xy / float2(1920.0, 1080.0);
    float phase = (float)(frame % 300u) / 300.0;
    return float4(frac(uv.x + phase), frac(uv.y + phase * 0.5),
                  frac(uv.x + uv.y + phase), 1.0);
}
)";

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

    HRESULT RenderBgra(std::uint64_t sequence, std::int64_t qpc) {
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
        context_->Unmap(constantBuffer_.Get(), 0);

        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width_),
            static_cast<float>(height_), 0.0F, 1.0F};
        ID3D11RenderTargetView* target = renderTarget_.Get();
        ID3D11Buffer* constantsBuffer = constantBuffer_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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
    std::uint64_t conversionCount_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
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
