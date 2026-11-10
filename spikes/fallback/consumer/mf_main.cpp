// SPDX-License-Identifier: Apache-2.0
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kFriendlyName[] = L"SparkFace Phase0 B2 Camera";

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
}

class MfGuard {
  public:
    MfGuard() { check_hresult(MFStartup(MF_VERSION), "MFStartup"); }
    ~MfGuard() { MFShutdown(); }
};

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
    return static_cast<double>(end - start) * 1000.0 / frequency;
}

ComPtr<IMFActivate> find_camera() {
    ComPtr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    check_hresult(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                  "IMFAttributes::SetGUID");
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    check_hresult(MFEnumDeviceSources(attributes.Get(), &activations, &count),
                  "MFEnumDeviceSources");
    ComPtr<IMFActivate> match;
    for (UINT32 index = 0; index < count; ++index) {
        wchar_t* name = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) {
            if (std::wstring_view(name).find(kFriendlyName) == 0) {
                match = activations[index];
                wchar_t* symbolic_link = nullptr;
                UINT32 symbolic_link_length = 0;
                if (SUCCEEDED(activations[index]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        &symbolic_link, &symbolic_link_length))) {
                    std::wcout << L"camera_symbolic_link=" << symbolic_link << L'\n';
                    CoTaskMemFree(symbolic_link);
                }
            }
            CoTaskMemFree(name);
        }
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    if (!match) {
        throw std::runtime_error("SparkFace Phase0 B2 Camera was not found");
    }
    return match;
}

std::uint64_t read_u64(const BYTE* data) {
    std::uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::int64_t read_i64(const BYTE* data) {
    std::int64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    return values[static_cast<std::size_t>(std::ceil(position))];
}

void print_distribution(const char* name, const std::vector<double>& values) {
    if (values.empty()) {
        std::cout << name << "_p50_ms=n/a\n"
                  << name << "_p95_ms=n/a\n"
                  << name << "_p99_ms=n/a\n"
                  << name << "_max_ms=n/a\n";
        return;
    }
    std::cout << name << "_p50_ms=" << percentile(values, 0.50) << '\n'
              << name << "_p95_ms=" << percentile(values, 0.95) << '\n'
              << name << "_p99_ms=" << percentile(values, 0.99) << '\n'
              << name << "_max_ms=" << *std::max_element(values.begin(), values.end()) << '\n';
}

struct Options {
    std::uint64_t frames = 1000;
    bool expect_recovery = false;
    std::string pacing_mode = "wait";
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--frames" && i + 1 < argc) {
            options.frames = std::stoull(argv[++i]);
        } else if (std::wstring_view(argv[i]) == L"--expect-recovery") {
            options.expect_recovery = true;
        } else if (std::wstring_view(argv[i]) == L"--pacing-mode" && i + 1 < argc) {
            const std::wstring_view mode(argv[++i]);
            if (mode == L"wait") {
                options.pacing_mode = "wait";
            } else if (mode == L"immediate") {
                options.pacing_mode = "immediate";
            } else {
                throw std::runtime_error("--pacing-mode must be wait or immediate");
            }
        } else {
            throw std::runtime_error(
                "Usage: fallback_mf_consumer [--frames N] [--expect-recovery] "
                "[--pacing-mode wait|immediate]");
        }
    }
    return options;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const std::uint64_t target = options.frames;
        MfGuard media_foundation;
        auto activation = find_camera();
        ComPtr<IMFMediaSource> source;
        check_hresult(activation->ActivateObject(IID_PPV_ARGS(&source)),
                      "IMFActivate::ActivateObject");

        ComPtr<IMFAttributes> reader_attributes;
        check_hresult(MFCreateAttributes(&reader_attributes, 2), "MFCreateAttributes(reader)");
        check_hresult(reader_attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE),
                      "Disable source-reader converters");
        ComPtr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromMediaSource(source.Get(), reader_attributes.Get(),
                                                          &reader),
                      "MFCreateSourceReaderFromMediaSource");

        ComPtr<IMFMediaType> type;
        check_hresult(MFCreateMediaType(&type), "MFCreateMediaType");
        check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set major type");
        check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Set subtype");
        check_hresult(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, 1920, 1080),
                      "Set frame size");
        check_hresult(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1),
                      "Set frame rate");
        constexpr DWORD video_stream =
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        check_hresult(reader->SetStreamSelection(video_stream, TRUE),
                      "SetStreamSelection");
        check_hresult(reader->SetCurrentMediaType(video_stream, nullptr, type.Get()),
                      "SetCurrentMediaType");

        std::vector<double> producer_to_publish_ms;
        std::vector<double> published_age_at_request_ms;
        std::vector<double> source_wait_ms;
        std::vector<double> source_copy_ms;
        std::vector<double> source_payload_finalization_gap_ms;
        std::vector<double> frame_server_client_delivery_ms;
        std::vector<double> producer_to_client_frame_age_ms;
        producer_to_publish_ms.reserve(static_cast<std::size_t>(target));
        published_age_at_request_ms.reserve(static_cast<std::size_t>(target));
        source_wait_ms.reserve(static_cast<std::size_t>(target));
        source_copy_ms.reserve(static_cast<std::size_t>(target));
        source_payload_finalization_gap_ms.reserve(static_cast<std::size_t>(target));
        frame_server_client_delivery_ms.reserve(static_cast<std::size_t>(target));
        producer_to_client_frame_age_ms.reserve(static_cast<std::size_t>(target));
        std::uint64_t received = 0;
        std::uint64_t last_sequence = 0;
        std::uint64_t skipped = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t idle_black_frames = 0;
        std::uint64_t marker_errors = 0;
        std::uint64_t published_after_request_sequences = 0;
        bool seen_valid = false;
        bool seen_idle_after_valid = false;
        bool recovered = false;
        std::int64_t last_valid_qpc = 0;
        double idle_black_ms = -1.0;
        while (received < target) {
            DWORD stream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            check_hresult(reader->ReadSample(video_stream, 0, &stream, &flags, &timestamp, &sample),
                          "IMFSourceReader::ReadSample");
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("Source reader reported MF_SOURCE_READERF_ERROR");
            }
            if (!sample) {
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            check_hresult(sample->ConvertToContiguousBuffer(&buffer),
                          "IMFSample::ConvertToContiguousBuffer");
            BYTE* bytes = nullptr;
            DWORD maximum = 0;
            DWORD current = 0;
            check_hresult(buffer->Lock(&bytes, &maximum, &current), "IMFMediaBuffer::Lock");
            if (current < 40) {
                ++marker_errors;
                buffer->Unlock();
                continue;
            }
            if (std::memcmp(bytes, "SFB2", 4) != 0) {
                const bool black = std::all_of(bytes, bytes + 40,
                                               [](BYTE value) { return value == 16; });
                if (black) {
                    ++idle_black_frames;
                    if (seen_valid && !seen_idle_after_valid) {
                        idle_black_ms = qpc_ms(last_valid_qpc, qpc_now());
                        seen_idle_after_valid = true;
                    }
                } else {
                    ++marker_errors;
                }
                buffer->Unlock();
                continue;
            }
            const std::uint64_t sequence = read_u64(bytes + 4);
            std::uint32_t check = 0;
            std::memcpy(&check, bytes + 12, sizeof(check));
            const std::uint32_t expected =
                static_cast<std::uint32_t>(sequence ^ (sequence >> 32) ^ 0xB2F00D5AU);
            const std::int64_t producer_qpc = read_i64(bytes + 16);
            const std::int64_t source_copy_complete_qpc = read_i64(bytes + 24);
            const std::int64_t published_qpc = read_i64(bytes + 32);
            const bool has_b3_diagnostics = current >= 72;
            const std::int64_t request_entry_qpc =
                has_b3_diagnostics ? read_i64(bytes + 40) : 0;
            const std::int64_t sequence_acquired_qpc =
                has_b3_diagnostics ? read_i64(bytes + 48) : 0;
            const std::int64_t copy_complete_qpc =
                has_b3_diagnostics ? read_i64(bytes + 56) : 0;
            const std::int64_t sample_payload_finalized_qpc =
                has_b3_diagnostics ? read_i64(bytes + 64) : 0;
            buffer->Unlock();
            if (check != expected || producer_qpc <= 0 ||
                published_qpc < producer_qpc || source_copy_complete_qpc < published_qpc) {
                ++marker_errors;
                continue;
            }
            const bool valid_b3_diagnostics = has_b3_diagnostics &&
                producer_qpc <= published_qpc &&
                published_qpc <= sequence_acquired_qpc &&
                request_entry_qpc <= sequence_acquired_qpc &&
                sequence_acquired_qpc <= copy_complete_qpc &&
                copy_complete_qpc <= sample_payload_finalized_qpc;
            if (seen_idle_after_valid && sequence < last_sequence) {
                last_sequence = 0;
            }
            if (last_sequence != 0 && sequence > last_sequence + 1) {
                skipped += sequence - last_sequence - 1;
            } else if (sequence == last_sequence) {
                ++duplicates;
            } else if (last_sequence != 0 && sequence < last_sequence) {
                ++marker_errors;
                continue;
            }
            last_sequence = sequence;
            const std::int64_t delivered_qpc = qpc_now();
            if (seen_idle_after_valid) {
                recovered = true;
            }
            seen_valid = true;
            last_valid_qpc = delivered_qpc;
            producer_to_publish_ms.push_back(qpc_ms(producer_qpc, published_qpc));
            if (valid_b3_diagnostics) {
                if (published_qpc <= request_entry_qpc) {
                    published_age_at_request_ms.push_back(qpc_ms(published_qpc, request_entry_qpc));
                } else {
                    ++published_after_request_sequences;
                }
                source_wait_ms.push_back(qpc_ms(request_entry_qpc, sequence_acquired_qpc));
                source_copy_ms.push_back(qpc_ms(sequence_acquired_qpc, copy_complete_qpc));
                source_payload_finalization_gap_ms.push_back(
                    qpc_ms(copy_complete_qpc, sample_payload_finalized_qpc));
            }
            const std::int64_t delivery_start_qpc = valid_b3_diagnostics
                ? sample_payload_finalized_qpc : source_copy_complete_qpc;
            frame_server_client_delivery_ms.push_back(qpc_ms(delivery_start_qpc, delivered_qpc));
            producer_to_client_frame_age_ms.push_back(qpc_ms(producer_qpc, delivered_qpc));
            ++received;
        }

        check_hresult(source->Shutdown(), "IMFMediaSource::Shutdown");
        activation->ShutdownObject();
        if (options.expect_recovery && (idle_black_frames < 3 || !recovered)) {
            throw std::runtime_error("Expected idle black frames and producer recovery");
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "mf_frames=" << received << '\n'
                  << "mf_pacing_mode=" << options.pacing_mode << '\n'
                  << "mf_b3_diagnostic_frames=" << source_wait_ms.size() << '\n'
                  << "mf_published_after_request_sequences="
                  << published_after_request_sequences << '\n'
                  << "mf_skipped_sequences=" << skipped << '\n'
                  << "mf_duplicate_sequences=" << duplicates << '\n'
                  << "mf_idle_black_frames=" << idle_black_frames << '\n'
                  << "idle_black_ms=" << idle_black_ms << '\n'
                  << "recovery=" << (recovered ? "yes" : "no") << '\n'
                  << "mf_marker_errors=" << marker_errors << '\n';
        print_distribution("producer_to_publish", producer_to_publish_ms);
        print_distribution("published_age_at_request", published_age_at_request_ms);
        print_distribution("source_wait", source_wait_ms);
        print_distribution("source_copy", source_copy_ms);
        print_distribution("source_copy_to_sample_payload_finalized",
                           source_payload_finalization_gap_ms);
        print_distribution("frame_server_client_delivery", frame_server_client_delivery_ms);
        print_distribution("producer_to_client_frame_age", producer_to_client_frame_age_ms);
        std::cout << "MF_CONSUMER_PASS\n";
        return marker_errors == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "mf_consumer_error=" << error.what() << '\n';
        return 1;
    }
}
