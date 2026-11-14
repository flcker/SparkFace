// SPDX-License-Identifier: Apache-2.0
// SparkFace Phase-0 transport-selection adapter for Microsoft Windows-Camera.
#pragma once

#include <Windows.h>
#include <mfapi.h>
#include <mfobjects.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>

#include "SharedRingProtocol.h"

namespace SparkFaceSharedBuffer {

inline void Trace(const char* message) {
    HANDLE file = CreateFileW(L"C:\\Users\\Public\\SparkFaceSharedBufferTrace.log",
                              FILE_APPEND_DATA,
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

class RingMapping final {
  public:
    static HRESULT Open(RingMapping** output) {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        const bool sourceOwned =
            GetFileAttributesW(SourceOwnedModePath) != INVALID_FILE_ATTRIBUTES;
        HANDLE file = CreateFileW(RingPath, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  sourceOwned ? OPEN_ALWAYS : OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (sourceOwned) {
            LARGE_INTEGER size{};
            size.QuadPart = static_cast<LONGLONG>(MappingBytes);
            if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) ||
                !SetEndOfFile(file)) {
                const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
                CloseHandle(file);
                return result;
            }
        }
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0,
                                            nullptr);
        if (mapping == nullptr) {
            const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(file);
            return result;
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                   MappingBytes);
        if (view == nullptr) {
            const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(mapping);
            CloseHandle(file);
            return result;
        }
        auto* header = static_cast<RingHeader*>(view);
        if (sourceOwned) {
            std::memset(view, 0, MappingBytes);
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency)) {
                UnmapViewOfFile(view);
                CloseHandle(mapping);
                CloseHandle(file);
                return HRESULT_FROM_WIN32(GetLastError());
            }
            header->magic = Magic;
            header->version = Version;
            header->width = Width;
            header->height = Height;
            header->frameBytes = static_cast<std::uint32_t>(FrameBytes);
            header->slotCount = SlotCount;
            header->ownerMode = 2;
            header->qpcFrequency = frequency.QuadPart;
            Trace("ring-created-by-source");
        }
        if (header->magic != Magic || header->version != Version ||
            header->width != Width || header->height != Height ||
            header->frameBytes != static_cast<std::uint32_t>(FrameBytes) ||
            header->slotCount != SlotCount) {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            CloseHandle(file);
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        }
        auto* state = new (std::nothrow) RingMapping(file, mapping, view);
        if (state == nullptr) {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            CloseHandle(file);
            return E_OUTOFMEMORY;
        }
        *output = state;
        return S_OK;
    }

    ULONG AddRef() { return static_cast<ULONG>(InterlockedIncrement(&references_)); }

    ULONG Release() {
        const ULONG remaining =
            static_cast<ULONG>(InterlockedDecrement(&references_));
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    [[nodiscard]] RingHeader* header() const {
        return static_cast<RingHeader*>(view_);
    }

    [[nodiscard]] void* view() const { return view_; }

  private:
    RingMapping(HANDLE file, HANDLE mapping, void* view)
        : file_(file), mapping_(mapping), view_(view) {}

    ~RingMapping() {
        UnmapViewOfFile(view_);
        CloseHandle(mapping_);
        CloseHandle(file_);
    }

    volatile LONG references_ = 1;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
};

class SharedMediaBuffer final : public IMFMediaBuffer, public IMF2DBuffer2 {
  public:
    SharedMediaBuffer(RingMapping* mapping, SlotHeader* slot)
        : mapping_(mapping), slot_(slot), currentLength_(FrameBytes) {
        mapping_->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId == __uuidof(IUnknown) ||
            interfaceId == __uuidof(IMFMediaBuffer)) {
            *object = static_cast<IMFMediaBuffer*>(this);
        } else if (interfaceId == __uuidof(IMF2DBuffer) ||
                   interfaceId == __uuidof(IMF2DBuffer2)) {
            *object = static_cast<IMF2DBuffer2*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining =
            static_cast<ULONG>(InterlockedDecrement(&references_));
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    STDMETHODIMP Lock(BYTE** buffer, DWORD* maximumLength,
                      DWORD* currentLength) override {
        if (buffer == nullptr) {
            return E_POINTER;
        }
        *buffer = reinterpret_cast<BYTE*>(slot_payload(slot_));
        if (maximumLength != nullptr) {
            *maximumLength = static_cast<DWORD>(FrameBytes);
        }
        if (currentLength != nullptr) {
            *currentLength = static_cast<DWORD>(currentLength_.load());
        }
        InterlockedIncrement64(&mapping_->header()->bufferLockCalls);
        return S_OK;
    }

    STDMETHODIMP Unlock() override { return S_OK; }

    STDMETHODIMP GetCurrentLength(DWORD* currentLength) override {
        if (currentLength == nullptr) {
            return E_POINTER;
        }
        *currentLength = static_cast<DWORD>(currentLength_.load());
        return S_OK;
    }

    STDMETHODIMP SetCurrentLength(DWORD currentLength) override {
        if (currentLength > FrameBytes) {
            return E_INVALIDARG;
        }
        currentLength_.store(currentLength);
        return S_OK;
    }

    STDMETHODIMP GetMaxLength(DWORD* maximumLength) override {
        if (maximumLength == nullptr) {
            return E_POINTER;
        }
        *maximumLength = static_cast<DWORD>(FrameBytes);
        return S_OK;
    }

    STDMETHODIMP Lock2D(BYTE** scanline0, LONG* pitch) override {
        if (scanline0 == nullptr || pitch == nullptr) {
            return E_POINTER;
        }
        *scanline0 = reinterpret_cast<BYTE*>(slot_payload(slot_));
        *pitch = static_cast<LONG>(Width);
        InterlockedIncrement64(&mapping_->header()->bufferLock2DCalls);
        return S_OK;
    }

    STDMETHODIMP Unlock2D() override { return S_OK; }

    STDMETHODIMP GetScanline0AndPitch(BYTE** scanline0, LONG* pitch) override {
        if (scanline0 == nullptr || pitch == nullptr) {
            return E_POINTER;
        }
        *scanline0 = reinterpret_cast<BYTE*>(slot_payload(slot_));
        *pitch = static_cast<LONG>(Width);
        return S_OK;
    }

    STDMETHODIMP IsContiguousFormat(BOOL* contiguous) override {
        if (contiguous == nullptr) {
            return E_POINTER;
        }
        *contiguous = TRUE;
        return S_OK;
    }

    STDMETHODIMP GetContiguousLength(DWORD* length) override {
        if (length == nullptr) {
            return E_POINTER;
        }
        *length = static_cast<DWORD>(FrameBytes);
        return S_OK;
    }

    STDMETHODIMP ContiguousCopyTo(BYTE* destination, DWORD length) override {
        if (destination == nullptr) {
            return E_POINTER;
        }
        if (length < FrameBytes) {
            return MF_E_BUFFERTOOSMALL;
        }
        std::memcpy(destination, slot_payload(slot_), FrameBytes);
        InterlockedIncrement64(&mapping_->header()->contiguousCopyToCalls);
        return S_OK;
    }

    STDMETHODIMP ContiguousCopyFrom(const BYTE* source, DWORD length) override {
        if (source == nullptr) {
            return E_POINTER;
        }
        if (length < FrameBytes) {
            return E_INVALIDARG;
        }
        std::memcpy(slot_payload(slot_), source, FrameBytes);
        InterlockedIncrement64(&mapping_->header()->contiguousCopyFromCalls);
        return S_OK;
    }

    STDMETHODIMP Lock2DSize(MF2DBuffer_LockFlags, BYTE** scanline0,
                            LONG* pitch, BYTE** bufferStart,
                            DWORD* bufferLength) override {
        if (scanline0 == nullptr || pitch == nullptr || bufferStart == nullptr ||
            bufferLength == nullptr) {
            return E_POINTER;
        }
        auto* payload = reinterpret_cast<BYTE*>(slot_payload(slot_));
        *scanline0 = payload;
        *pitch = static_cast<LONG>(Width);
        *bufferStart = payload;
        *bufferLength = static_cast<DWORD>(FrameBytes);
        InterlockedIncrement64(&mapping_->header()->bufferLock2DCalls);
        return S_OK;
    }

    STDMETHODIMP Copy2DTo(IMF2DBuffer2* destination) override {
        if (destination == nullptr) {
            return E_POINTER;
        }
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        BYTE* bufferStart = nullptr;
        DWORD bufferLength = 0;
        RETURN_IF_FAILED(destination->Lock2DSize(
            MF2DBuffer_LockFlags_Write, &scanline, &pitch, &bufferStart,
            &bufferLength));
        HRESULT result = S_OK;
        const auto absolutePitch = static_cast<DWORD>(pitch < 0 ? -pitch : pitch);
        constexpr DWORD rowCount = Height * 3 / 2;
        if (absolutePitch >= Width && bufferLength >= absolutePitch * rowCount) {
            const auto* source = reinterpret_cast<const BYTE*>(slot_payload(slot_));
            for (DWORD row = 0; row < rowCount; ++row) {
                std::memcpy(scanline + static_cast<std::ptrdiff_t>(row) * pitch,
                            source + static_cast<std::size_t>(row) * Width, Width);
            }
        } else {
            result = MF_E_BUFFERTOOSMALL;
        }
        const HRESULT unlockResult = destination->Unlock2D();
        InterlockedIncrement64(&mapping_->header()->copy2DToCalls);
        return FAILED(result) ? result : unlockResult;
    }

  private:
    ~SharedMediaBuffer() {
        InterlockedDecrement(&slot_->leaseCount);
        InterlockedIncrement64(&mapping_->header()->releasedBufferCount);
        mapping_->Release();
    }

    volatile LONG references_ = 1;
    RingMapping* mapping_ = nullptr;
    SlotHeader* slot_ = nullptr;
    std::atomic<DWORD> currentLength_;
};

class SampleSource final {
  public:
    SampleSource() = default;
    ~SampleSource() {
        if (mapping_ != nullptr) {
            mapping_->Release();
        }
    }
    SampleSource(const SampleSource&) = delete;
    SampleSource& operator=(const SampleSource&) = delete;

    HRESULT CreateLatestSample(IMFSample** output, bool* delivered) {
        if (output == nullptr || delivered == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        *delivered = false;
        if (mapping_ == nullptr) {
            const HRESULT openResult = RingMapping::Open(&mapping_);
            if (FAILED(openResult)) {
                return S_OK;
            }
            char image[MAX_PATH]{};
            GetModuleFileNameA(nullptr, image, MAX_PATH);
            char message[MAX_PATH + 64]{};
            sprintf_s(message, "ring-open pid=%lu image=%s", GetCurrentProcessId(),
                      image);
            Trace(message);
        }

        auto* header = mapping_->header();
        const LONG64 latest = atomic_load(&header->publishedSequence);
        if (latest <= 0 || latest == lastSequence_) {
            return S_OK;
        }

        SlotHeader* selected = nullptr;
        for (std::uint32_t index = 0; index < SlotCount; ++index) {
            auto* slot = slot_header(mapping_->view(), index);
            if (atomic_load(&slot->sequence) != latest) {
                continue;
            }
            if (InterlockedCompareExchange(&slot->leaseCount, 1, 0) != 0) {
                continue;
            }
            MemoryBarrier();
            if (atomic_load(&slot->sequence) == latest) {
                selected = slot;
                break;
            }
            InterlockedExchange(&slot->leaseCount, 0);
        }
        if (selected == nullptr) {
            return S_OK;
        }

        auto* buffer = new (std::nothrow) SharedMediaBuffer(mapping_, selected);
        if (buffer == nullptr) {
            InterlockedDecrement(&selected->leaseCount);
            return E_OUTOFMEMORY;
        }
        IMFSample* sample = nullptr;
        HRESULT result = MFCreateSample(&sample);
        if (SUCCEEDED(result)) {
            result = sample->AddBuffer(buffer);
        }
        buffer->Release();
        if (FAILED(result)) {
            if (sample != nullptr) {
                sample->Release();
            }
            return result;
        }

        lastSequence_ = latest;
        *output = sample;
        *delivered = true;
        return S_OK;
    }

  private:
    RingMapping* mapping_ = nullptr;
    LONG64 lastSequence_ = 0;
};

} // namespace SparkFaceSharedBuffer
