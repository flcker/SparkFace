// SPDX-License-Identifier: Apache-2.0
// SparkFace Phase-0 direct compressed-subtype capability probe.
#pragma once

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <vector>

namespace SparkFaceCompressed {

inline constexpr wchar_t ModePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportCompressed.mode";
inline constexpr wchar_t FramePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportCompressed.frame";

class FrameSource final {
  public:
    enum class Mode { Disabled, H264, Mjpeg };

    HRESULT Initialize() {
        mode_ = ReadMode();
        if (mode_ == Mode::Disabled) {
            return S_OK;
        }
        HANDLE file = CreateFileW(FramePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
            size.QuadPart > 4 * 1024 * 1024) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return error == ERROR_SUCCESS ? E_INVALIDARG : HRESULT_FROM_WIN32(error);
        }
        frame_.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        const BOOL readResult = ReadFile(file, frame_.data(),
                                         static_cast<DWORD>(frame_.size()), &read,
                                         nullptr);
        const DWORD error = GetLastError();
        CloseHandle(file);
        if (!readResult || read != frame_.size()) {
            return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_READ_FAULT : error);
        }
        return S_OK;
    }

    [[nodiscard]] bool enabled() const { return mode_ != Mode::Disabled; }

    HRESULT ConfigureMediaType(IMFMediaType* mediaType) const {
        if (mediaType == nullptr) {
            return E_POINTER;
        }
        if (!enabled()) {
            return mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        }
        const GUID& subtype = mode_ == Mode::H264 ? MFVideoFormat_H264_ES
                                                  : MFVideoFormat_MJPG;
        RETURN_IF_FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, subtype));
        RETURN_IF_FAILED(mediaType->SetUINT32(MF_MT_COMPRESSED, TRUE));
        RETURN_IF_FAILED(mediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, FALSE));
        RETURN_IF_FAILED(mediaType->SetUINT32(
            MF_MT_SAMPLE_SIZE, static_cast<UINT32>(frame_.size())));
        return S_OK;
    }

    HRESULT CreateSample(IMFSample** output, bool* delivered) {
        if (output == nullptr || delivered == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        *delivered = false;
        if (!enabled() || delivered_) {
            return S_OK;
        }
        IMFMediaBuffer* buffer = nullptr;
        RETURN_IF_FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame_.size()),
                                              &buffer));
        BYTE* bytes = nullptr;
        HRESULT result = buffer->Lock(&bytes, nullptr, nullptr);
        if (SUCCEEDED(result)) {
            std::memcpy(bytes, frame_.data(), frame_.size());
            result = buffer->Unlock();
        }
        if (SUCCEEDED(result)) {
            result = buffer->SetCurrentLength(static_cast<DWORD>(frame_.size()));
        }
        IMFSample* sample = nullptr;
        if (SUCCEEDED(result)) {
            result = MFCreateSample(&sample);
        }
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
        delivered_ = true;
        *output = sample;
        *delivered = true;
        return S_OK;
    }

  private:
    static Mode ReadMode() {
        HANDLE file = CreateFileW(ModePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return Mode::Disabled;
        }
        char value[16]{};
        DWORD read = 0;
        const BOOL result = ReadFile(file, value, sizeof(value) - 1, &read, nullptr);
        CloseHandle(file);
        if (!result) {
            return Mode::Disabled;
        }
        if (std::strncmp(value, "h264", 4) == 0) {
            return Mode::H264;
        }
        if (std::strncmp(value, "mjpeg", 5) == 0) {
            return Mode::Mjpeg;
        }
        return Mode::Disabled;
    }

    Mode mode_ = Mode::Disabled;
    std::vector<std::uint8_t> frame_;
    bool delivered_ = false;
};

} // namespace SparkFaceCompressed
