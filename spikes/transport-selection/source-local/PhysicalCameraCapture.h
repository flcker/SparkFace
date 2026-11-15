// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")
namespace SparkFaceSourceLocal {

class PhysicalCameraCapture final {
  public:
    PhysicalCameraCapture() = default;
    class Frame final {
      public:
        Frame() = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        ~Frame() { Reset(); }

        void Reset() {
            if (owner_ != nullptr) {
                owner_->ReleaseFrame(this);
            }
        }
        [[nodiscard]] ID3D11Texture2D* texture() const {
            return texture_.Get();
        }
        [[nodiscard]] UINT subresource() const { return subresource_; }
        [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
        [[nodiscard]] std::int64_t capture_qpc() const { return captureQpc_; }

      private:
        friend class PhysicalCameraCapture;
        PhysicalCameraCapture* owner_ = nullptr;
        Microsoft::WRL::ComPtr<IMFSample> sample_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        UINT subresource_ = 0;
        std::uint64_t sequence_ = 0;
        std::int64_t captureQpc_ = 0;
    };

    PhysicalCameraCapture(const PhysicalCameraCapture&) = delete;
    PhysicalCameraCapture& operator=(const PhysicalCameraCapture&) = delete;

    ~PhysicalCameraCapture() { Shutdown(); }

    HRESULT Initialize(ID3D11Device* device, const wchar_t* symbolicLink,
                       UINT32 symbolicLinkLength, UINT32 width, UINT32 height,
                       UINT32 frameRate, bool verboseDiagnostics = true) {
        if (device == nullptr || symbolicLink == nullptr ||
            symbolicLinkLength == 0 || width == 0 || height == 0 ||
            frameRate == 0) {
            return E_INVALIDARG;
        }
        if (reader_ != nullptr) {
            return MF_E_INVALIDREQUEST;
        }

        device_ = device;
        width_ = width;
        height_ = height;
        verboseDiagnostics_ = verboseDiagnostics;
        frameRate_ = frameRate;
        sampleReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (sampleReadyEvent_ == nullptr) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        sampleConsumedEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (sampleConsumedEvent_ == nullptr) {
            const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(sampleReadyEvent_);
            sampleReadyEvent_ = nullptr;
            return result;
        }

        callbackResult_ = E_PENDING;
        callbackFlags_ = 0;
        callbackSequence_ = 0;
        callbackQpc_ = 0;
        HRESULT result = MFCreateDXGIDeviceManager(&deviceManagerToken_,
                                                   &deviceManager_);
        if (SUCCEEDED(result)) {
            result = deviceManager_->ResetDevice(device_.Get(),
                                                  deviceManagerToken_);
        }


        Microsoft::WRL::ComPtr<IMFAttributes> sourceAttributes;
        if (SUCCEEDED(result)) {
            result = MFCreateAttributes(&sourceAttributes, 2);
        }
        if (SUCCEEDED(result)) {
            result = sourceAttributes->SetGUID(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        }
        if (SUCCEEDED(result)) {
            result = sourceAttributes->SetString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                symbolicLink);
        }
        if (SUCCEEDED(result)) {
            result = MFCreateDeviceSource(sourceAttributes.Get(), &mediaSource_);
        }

        Microsoft::WRL::ComPtr<IMFAttributes> readerAttributes;
        if (SUCCEEDED(result)) {
            result = MFCreateAttributes(&readerAttributes, 3);
        }
        if (SUCCEEDED(result)) {
            result = readerAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                                  deviceManager_.Get());
        }
        if (SUCCEEDED(result)) {
            result = readerAttributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        if (SUCCEEDED(result)) {
            result = readerAttributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        if (SUCCEEDED(result)) {
            result = MFCreateSourceReaderFromMediaSource(
                mediaSource_.Get(), readerAttributes.Get(), &reader_);
        }

        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        if (SUCCEEDED(result)) {
            result = MFCreateMediaType(&outputType);
        }
        if (SUCCEEDED(result)) {
            result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        }
        if (SUCCEEDED(result)) {
            result = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE,
                                        width_, height_);
        }
        if (SUCCEEDED(result)) {
            result = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE,
                                         frameRate_, 1);
        }
        if (SUCCEEDED(result)) {
            result = reader_->SetCurrentMediaType(StreamIndex, nullptr,
                                                  outputType.Get());
        }
        if (SUCCEEDED(result)) {
            result = RecordNegotiatedType();
        }
        if (SUCCEEDED(result)) {
            result = TraceDevice("allocator", device_.Get(), &allocatorLuid_);
        }
        if (SUCCEEDED(result)) {
            result = StartCaptureThread();
        }
        if (FAILED(result)) {
            TraceResult("physical_camera_initialize_hresult", result);
            Shutdown();
            return result;
        }
        TraceResult("physical_camera_initialize_hresult", S_OK);
        return S_OK;
    }

    HRESULT AcquireNext(Frame* frame) {
        if (frame == nullptr) {
            return E_POINTER;
        }
        if (frame->owner_ != nullptr) {
            return MF_E_INVALIDREQUEST;
        }
        if (reader_ == nullptr || sampleReadyEvent_ == nullptr) {
            return MF_E_NOT_INITIALIZED;
        }

        const DWORD wait = WaitForSingleObject(sampleReadyEvent_, 2000);
        if (wait != WAIT_OBJECT_0) {
            const HRESULT result = wait == WAIT_TIMEOUT
                                       ? HRESULT_FROM_WIN32(WAIT_TIMEOUT)
                                       : HRESULT_FROM_WIN32(GetLastError());
            TraceResult("physical_camera_wait_hresult", result);
            TraceDeviceStatus("allocator_wait", device_.Get());
            return result;
        }

        Microsoft::WRL::ComPtr<IMFSample> sample;
        HRESULT callbackResult = E_PENDING;
        DWORD callbackFlags = 0;
        std::uint64_t frameSequence = 0;
        std::int64_t frameQpc = 0;
        AcquireSRWLockExclusive(&lock_);
        callbackResult = callbackResult_;
        callbackFlags = callbackFlags_;
        sample = sample_;
        if (sample != nullptr) {
            sample_.Reset();
            frameSequence = callbackSequence_;
            frameQpc = callbackQpc_;
        }
        ReleaseSRWLockExclusive(&lock_);

        if (sample == nullptr) {
            if (callbackResult != E_PENDING && FAILED(callbackResult)) {
                TraceResult("physical_camera_callback_hresult", callbackResult);
                return callbackResult;
            }
            if ((callbackFlags & MF_SOURCE_READERF_ERROR) != 0) {
                return MF_E_HW_MFT_FAILED_START_STREAMING;
            }
            return E_PENDING;
        }

        HRESULT result = S_OK;
        if ((callbackFlags & MF_SOURCE_READERF_ERROR) != 0) {
            result = MF_E_HW_MFT_FAILED_START_STREAMING;
        }
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(result)) {
            result = sample->GetBufferByIndex(0, &buffer);
        }
        Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
        if (SUCCEEDED(result)) {
            result = buffer.As(&dxgiBuffer);
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> capturedTexture;
        if (SUCCEEDED(result)) {
            result = dxgiBuffer->GetResource(IID_PPV_ARGS(&capturedTexture));
        }
        UINT capturedSubresource = 0;
        if (SUCCEEDED(result)) {
            result = dxgiBuffer->GetSubresourceIndex(&capturedSubresource);
        }
        D3D11_TEXTURE2D_DESC description{};
        if (SUCCEEDED(result)) {
            capturedTexture->GetDesc(&description);
            if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
                description.Width != width_ || description.Height != height_) {
                result = MF_E_UNSUPPORTED_FORMAT;
            }
        }
        Microsoft::WRL::ComPtr<ID3D11Device> captureDevice;
        LUID captureLuid{};
        if (SUCCEEDED(result)) {
            capturedTexture->GetDevice(&captureDevice);
            result = verboseDiagnostics_ || frameSequence <= 5 ||
                             (frameSequence % 100) == 0
                         ? TraceDevice("capture", captureDevice.Get(), &captureLuid)
                         : QueryDeviceLuid(captureDevice.Get(), &captureLuid);
        }
        if (SUCCEEDED(result) &&
            (captureLuid.HighPart != allocatorLuid_.HighPart ||
             captureLuid.LowPart != allocatorLuid_.LowPart)) {
            result = MF_E_UNSUPPORTED_D3D_TYPE;
        }
        if (SUCCEEDED(result)) {
            frame->owner_ = this;
            frame->sample_ = sample;
            frame->texture_ = capturedTexture;
            frame->device_ = captureDevice;
            frame->subresource_ = capturedSubresource;
            frame->sequence_ = frameSequence;
            frame->captureQpc_ = frameQpc;
        } else {
            sample.Reset();
            SetEvent(sampleConsumedEvent_);
            TraceResult("physical_camera_acquire_hresult", result);
            TraceDeviceStatus("allocator_acquire", device_.Get());
            TraceDeviceStatus("capture_acquire", captureDevice.Get());
        }
        return result;
    }

    void Shutdown() {
        running_.store(false, std::memory_order_release);
        if (sampleConsumedEvent_ != nullptr) {
            SetEvent(sampleConsumedEvent_);
        }
        if (reader_ != nullptr) {
            (void)reader_->Flush(StreamIndex);
        }
        if (mediaSource_ != nullptr) {
            (void)mediaSource_->Shutdown();
        }
        if (captureThread_.joinable()) {
            captureThread_.join();
        }
        AcquireSRWLockExclusive(&lock_);
        sample_.Reset();
        ReleaseSRWLockExclusive(&lock_);
        reader_.Reset();
        mediaSource_.Reset();
        deviceManager_.Reset();
        device_.Reset();
        if (sampleReadyEvent_ != nullptr) {
            CloseHandle(sampleReadyEvent_);
            sampleReadyEvent_ = nullptr;
        }
        if (sampleConsumedEvent_ != nullptr) {
            CloseHandle(sampleConsumedEvent_);
            sampleConsumedEvent_ = nullptr;
        }
        deviceManagerToken_ = 0;
    }

    [[nodiscard]] GUID subtype() const { return subtype_; }
    [[nodiscard]] LUID allocator_luid() const { return allocatorLuid_; }

  private:
    static constexpr DWORD StreamIndex =
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    static constexpr wchar_t TracePath[] =
        L"C:\\Users\\Public\\SparkFaceSourceLocalTrace.log";

    HRESULT StartCaptureThread() {
        running_.store(true, std::memory_order_release);
        try {
            captureThread_ = std::thread(&PhysicalCameraCapture::CaptureLoop,
                                         this);
        } catch (...) {
            running_.store(false, std::memory_order_release);
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    void CaptureLoop() {
        const HRESULT apartmentResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartmentResult)) {
            AcquireSRWLockExclusive(&lock_);
            callbackResult_ = apartmentResult;
            ReleaseSRWLockExclusive(&lock_);
            SetEvent(sampleReadyEvent_);
            return;
        }

        while (running_.load(std::memory_order_acquire)) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            Microsoft::WRL::ComPtr<IMFSample> sample;
            HRESULT result = reader_->ReadSample(
                StreamIndex, 0, &actualStream, &flags, &timestamp, &sample);
            LARGE_INTEGER qpc{};
            if (!QueryPerformanceCounter(&qpc) && SUCCEEDED(result)) {
                result = HRESULT_FROM_WIN32(GetLastError());
            }

            const bool framePublished = SUCCEEDED(result) && sample != nullptr;
            std::uint64_t sequence = 0;
            AcquireSRWLockExclusive(&lock_);
            callbackResult_ = result;
            callbackFlags_ = flags;
            if (framePublished) {
                sample_ = sample;
                callbackQpc_ = qpc.QuadPart;
                sequence = ++callbackSequence_;
            }
            ReleaseSRWLockExclusive(&lock_);
            sample.Reset();
            if (verboseDiagnostics_ || FAILED(result) || sequence <= 5 ||
                (sequence % 100) == 0) {
                TraceFormat(
                    "physical_camera_read sequence=%llu thread=%lu stream=%lu flags=0x%08lx timestamp=%lld sample=%u hresult=0x%08lx",
                    static_cast<unsigned long long>(sequence),
                    static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long>(actualStream),
                    static_cast<unsigned long>(flags),
                    static_cast<long long>(timestamp), framePublished ? 1U : 0U,
                    static_cast<unsigned long>(result));
            }
            SetEvent(sampleReadyEvent_);

            if (FAILED(result) ||
                (flags & (MF_SOURCE_READERF_ERROR |
                          MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
                TraceDeviceStatus("allocator_read", device_.Get());
                break;
            }
            if (framePublished &&
                WaitForSingleObject(sampleConsumedEvent_, INFINITE) !=
                    WAIT_OBJECT_0) {
                break;
            }
        }
        CoUninitialize();
    }

    HRESULT RecordNegotiatedType() {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        HRESULT result = reader_->GetCurrentMediaType(StreamIndex, &type);
        if (SUCCEEDED(result)) {
            result = type->GetGUID(MF_MT_SUBTYPE, &subtype_);
        }
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 frameRateNumerator = 0;
        UINT32 frameRateDenominator = 0;
        if (SUCCEEDED(result)) {
            result = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width,
                                        &height);
        }
        if (SUCCEEDED(result)) {
            result = MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                         &frameRateNumerator,
                                         &frameRateDenominator);
        }
        if (SUCCEEDED(result) && (width != width_ || height != height_)) {
            result = MF_E_INVALIDMEDIATYPE;
        }
        wchar_t subtype[64]{};
        StringFromGUID2(subtype_, subtype, static_cast<int>(std::size(subtype)));
        TraceFormat(
            "physical_camera_media_type subtype=%ls width=%u height=%u frame_rate=%u/%u hresult=0x%08lx",
            subtype, width, height, frameRateNumerator, frameRateDenominator,
            static_cast<unsigned long>(result));
        return result;
    }

    static HRESULT QueryDeviceLuid(ID3D11Device* device, LUID* luid) {
        if (device == nullptr || luid == nullptr) {
            return E_POINTER;
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(result)) {
            result = dxgiDevice->GetAdapter(&adapter);
        }
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(result)) {
            result = adapter->GetDesc(&description);
        }
        if (SUCCEEDED(result)) {
            *luid = description.AdapterLuid;
        }
        return result;
    }

    static HRESULT TraceDevice(const char* name, ID3D11Device* device,
                               LUID* luid) {
        const HRESULT result = QueryDeviceLuid(device, luid);
        const HRESULT removed = device == nullptr
                                    ? E_POINTER
                                    : device->GetDeviceRemovedReason();
        TraceFormat(
            "physical_camera_device name=%s pointer=%p luid=%08lx:%08lx removed_reason=0x%08lx query_hresult=0x%08lx",
            name, static_cast<void*>(device),
            static_cast<unsigned long>(luid->HighPart),
            static_cast<unsigned long>(luid->LowPart),
            static_cast<unsigned long>(removed),
            static_cast<unsigned long>(result));
        return result;
    }

    static void TraceDeviceStatus(const char* name, ID3D11Device* device) {
        if (device == nullptr) {
            TraceFormat("physical_camera_device_status name=%s pointer=null", name);
            return;
        }
        TraceFormat(
            "physical_camera_device_status name=%s pointer=%p removed_reason=0x%08lx",
            name, static_cast<void*>(device),
            static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            TraceFormat("physical_camera_info_queue name=%s available=0", name);
            return;
        }
        const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        TraceFormat("physical_camera_info_queue name=%s available=1 messages=%llu",
                    name, static_cast<unsigned long long>(count));
        const UINT64 first = count > 16 ? count - 16 : 0;
        for (UINT64 index = first; index < count; ++index) {
            std::array<BYTE, 4096> storage{};
            SIZE_T bytes = storage.size();
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(index, message, &bytes)) &&
                message->pDescription != nullptr) {
                TraceFormat(
                    "physical_camera_info_message index=%llu severity=%u id=%u description=%s",
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned>(message->Severity),
                    static_cast<unsigned>(message->ID), message->pDescription);
            }
        }
    }

    void ReleaseFrame(Frame* frame) {
        frame->texture_.Reset();
        frame->device_.Reset();
        frame->sample_.Reset();
        frame->owner_ = nullptr;
        frame->subresource_ = 0;
        frame->sequence_ = 0;
        frame->captureQpc_ = 0;
        SetEvent(sampleConsumedEvent_);
    }

    static void TraceFormat(const char* format, ...) {
        char message[1024]{};
        va_list arguments;
        va_start(arguments, format);
        vsprintf_s(message, format, arguments);
        va_end(arguments);
        strcat_s(message, "\r\n");
        HANDLE file = CreateFileW(TracePath, FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD written = 0;
        WriteFile(file, message, static_cast<DWORD>(std::strlen(message)),
                  &written, nullptr);
        CloseHandle(file);
    }

    static void TraceResult(const char* name, HRESULT result) {
        TraceFormat("%s=0x%08lx", name, static_cast<unsigned long>(result));
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    HANDLE sampleReadyEvent_ = nullptr;
    HANDLE sampleConsumedEvent_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread captureThread_;
    HRESULT callbackResult_ = E_PENDING;
    DWORD callbackFlags_ = 0;
    std::uint64_t callbackSequence_ = 0;
    std::int64_t callbackQpc_ = 0;
    UINT32 width_ = 0;
    UINT32 height_ = 0;
    UINT32 frameRate_ = 0;
    UINT deviceManagerToken_ = 0;
    bool verboseDiagnostics_ = true;
    GUID subtype_{};
    LUID allocatorLuid_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFSample> sample_;
};

} // namespace SparkFaceSourceLocal
