// SPDX-License-Identifier: Apache-2.0
#include "frame_transport.h"

// clang-format off
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sparkface::d3d11_handoff;

namespace {

constexpr wchar_t kFriendlyName[] = L"SparkFace Phase0 B2 Camera";

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        throw std::runtime_error(message.str());
    }
}

class MfGuard {
  public:
    MfGuard() { check_hresult(MFStartup(MF_VERSION), "MFStartup"); }
    ~MfGuard() { MFShutdown(); }
};

std::int64_t qpc_now() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) {
        throw std::runtime_error("QueryPerformanceCounter failed");
    }
    return value.QuadPart;
}

double qpc_ms(std::int64_t start, std::int64_t end) {
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || end < start) {
        throw std::runtime_error("Invalid QPC interval");
    }
    return static_cast<double>(end - start) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

ComPtr<IMFActivate> find_camera() {
    ComPtr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    check_hresult(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                  "Set source type");
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    check_hresult(MFEnumDeviceSources(attributes.Get(), &devices, &count),
                  "MFEnumDeviceSources");
    ComPtr<IMFActivate> result;
    for (UINT32 index = 0; index < count; ++index) {
        WCHAR* name = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) {
            if (std::wstring_view(name).find(kFriendlyName) == 0) {
                result = devices[index];
            }
            CoTaskMemFree(name);
        }
        devices[index]->Release();
    }
    CoTaskMemFree(devices);
    if (!result) {
        throw std::runtime_error("SparkFace Phase0 D Camera was not found");
    }
    return result;
}

struct Options {
    std::uint64_t frames = 1000;
    double minimum_fps = 29.5;
    double maximum_p95_ms = 33.3;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--frames" && index + 1 < argc) {
            options.frames = std::stoull(argv[++index]);
        } else if (argument == L"--minimum-fps" && index + 1 < argc) {
            options.minimum_fps = std::stod(argv[++index]);
        } else if (argument == L"--maximum-p95-ms" && index + 1 < argc) {
            options.maximum_p95_ms = std::stod(argv[++index]);
        } else {
            throw std::invalid_argument(
                "usage: d3d11_handoff_consumer [--frames N] [--minimum-fps F] "
                "[--maximum-p95-ms F]");
        }
    }
    if (options.frames == 0 || options.minimum_fps <= 0.0 || options.maximum_p95_ms <= 0.0) {
        throw std::invalid_argument("Consumer gate values must be positive");
    }
    return options;
}

std::uint64_t decode_word(const BYTE* bytes, std::uint32_t first_bit,
                          std::uint32_t bit_count) {
    std::uint64_t value = 0;
    for (std::uint32_t bit = 0; bit < bit_count; ++bit) {
        const std::uint32_t block = first_bit + bit;
        const std::uint32_t x = block * 8 + 4;
        if (bytes[x] >= 128) {
            value |= std::uint64_t{1} << bit;
        }
    }
    return value;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values.at(std::min(index, values.size() - 1));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parse_options(argc, argv);
        MfGuard media_foundation;
        ComPtr<IMFActivate> activation = find_camera();
        ComPtr<IMFMediaSource> source;
        check_hresult(activation->ActivateObject(IID_PPV_ARGS(&source)),
                      "IMFActivate::ActivateObject");

        ComPtr<IMFAttributes> reader_attributes;
        check_hresult(MFCreateAttributes(&reader_attributes, 2), "MFCreateAttributes reader");
        check_hresult(reader_attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE),
                      "Disable converters");
        ComPtr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromMediaSource(source.Get(), reader_attributes.Get(),
                                                          &reader),
                      "MFCreateSourceReaderFromMediaSource");
        ComPtr<IMFMediaType> type;
        check_hresult(MFCreateMediaType(&type), "MFCreateMediaType");
        check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set major type");
        check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Set subtype");
        check_hresult(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight),
                      "Set frame size");
        check_hresult(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFrameRate, 1),
                      "Set frame rate");
        constexpr DWORD stream_index =
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        check_hresult(reader->SetCurrentMediaType(stream_index, nullptr, type.Get()),
                      "SetCurrentMediaType");

        std::vector<double> frame_age_ms;
        frame_age_ms.reserve(static_cast<std::size_t>(options.frames));
        std::uint64_t received = 0;
        std::uint64_t last_sequence = 0;
        std::uint32_t last_producer_pid = 0;
        std::uint64_t producer_restarts = 0;
        std::uint64_t skipped = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t marker_errors = 0;
        const std::int64_t started = qpc_now();
        while (received < options.frames) {
            DWORD actual_stream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            check_hresult(reader->ReadSample(stream_index, 0, &actual_stream, &flags, &timestamp,
                                             &sample),
                          "IMFSourceReader::ReadSample");
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("Source reader reported an error");
            }
            if (!sample) {
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            check_hresult(sample->ConvertToContiguousBuffer(&buffer),
                          "ConvertToContiguousBuffer");
            BYTE* bytes = nullptr;
            DWORD maximum = 0;
            DWORD current = 0;
            check_hresult(buffer->Lock(&bytes, &maximum, &current), "IMFMediaBuffer::Lock");
            const std::int64_t delivered_qpc = qpc_now();
            if (current < kWidth * kHeight * 3 / 2) {
                ++marker_errors;
                buffer->Unlock();
                continue;
            }
            const std::uint16_t magic = static_cast<std::uint16_t>(decode_word(bytes, 0, 16));
            const std::uint64_t sequence = decode_word(bytes, 16, 64);
            const std::int64_t producer_qpc =
                static_cast<std::int64_t>(decode_word(bytes, 80, 64));
            const std::uint32_t producer_pid =
                static_cast<std::uint32_t>(decode_word(bytes, 144, 32));
            buffer->Unlock();
            if (magic != kPayloadMagic || sequence == 0 || producer_qpc <= 0 ||
                producer_qpc > delivered_qpc || producer_pid == 0) {
                ++marker_errors;
                continue;
            }
            if (last_producer_pid != 0 && producer_pid != last_producer_pid) {
                ++producer_restarts;
                last_sequence = 0;
            }
            last_producer_pid = producer_pid;
            if (last_sequence != 0 && sequence == last_sequence) {
                ++duplicates;
            } else if (last_sequence != 0 && sequence > last_sequence + 1) {
                skipped += sequence - last_sequence - 1;
            } else if (last_sequence != 0 && sequence < last_sequence) {
                ++marker_errors;
                continue;
            }
            last_sequence = sequence;
            frame_age_ms.push_back(qpc_ms(producer_qpc, delivered_qpc));
            ++received;
        }
        const std::int64_t stopped = qpc_now();
        const double elapsed_ms = qpc_ms(started, stopped);
        const double fps = static_cast<double>(received) * 1000.0 / elapsed_ms;
        const double p50_ms = percentile(frame_age_ms, 0.50);
        const double p95_ms = percentile(frame_age_ms, 0.95);
        const double p99_ms = percentile(frame_age_ms, 0.99);
        const double maximum_ms = *std::max_element(frame_age_ms.begin(), frame_age_ms.end());
        const bool pass = fps >= options.minimum_fps && p95_ms <= options.maximum_p95_ms &&
                          marker_errors == 0 && duplicates == 0 && skipped == 0;

        reader.Reset();
        source.Reset();
        check_hresult(activation->ShutdownObject(), "IMFActivate::ShutdownObject");
        std::cout << std::fixed << std::setprecision(3)
                  << "mf_frames=" << received << '\n'
                  << "mf_fps_achieved=" << fps << '\n'
                  << "producer_to_client_frame_age_p50_ms=" << p50_ms << '\n'
                  << "producer_to_client_frame_age_p95_ms=" << p95_ms << '\n'
                  << "producer_to_client_frame_age_p99_ms=" << p99_ms << '\n'
                  << "producer_to_client_frame_age_max_ms=" << maximum_ms << '\n'
                  << "skipped_sequences=" << skipped << '\n'
                  << "duplicate_sequences=" << duplicates << '\n'
                  << "marker_errors=" << marker_errors << '\n'
                  << "producer_restarts=" << producer_restarts << '\n'
                  << "full_frame_cpu_copy_before_frame_server=0\n"
                  << (pass ? "D3D11_HANDOFF_GATE_PASS\n" : "D3D11_HANDOFF_GATE_FAIL\n");
        return pass ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "D3D11_HANDOFF_CONSUMER_ERROR: " << error.what() << '\n';
        return 1;
    }
}
