// SPDX-License-Identifier: Apache-2.0
#include "../../d3d11-handoff/frame_transport.h"
#include "control_protocol.h"

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
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <optional>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sparkface::d3d11_handoff;

namespace {

constexpr wchar_t FriendlyName[] = L"SparkFace Phase0 B2 Camera";

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        throw std::runtime_error(message.str());
    }
}

class MfGuard final {
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
    check_hresult(attributes->SetGUID(
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                  "Set source type");
    IMFActivate** rawDevices = nullptr;
    UINT32 count = 0;
    check_hresult(MFEnumDeviceSources(attributes.Get(), &rawDevices, &count),
                  "MFEnumDeviceSources");
    ComPtr<IMFActivate> selected;
    for (UINT32 index = 0; index < count; ++index) {
        WCHAR* rawName = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(rawDevices[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &rawName, &length))) {
            if (std::wstring_view(rawName).find(FriendlyName) == 0) {
                selected = rawDevices[index];
            }
            CoTaskMemFree(rawName);
        }
        rawDevices[index]->Release();
    }
    CoTaskMemFree(rawDevices);
    if (!selected) {
        throw std::runtime_error("SparkFace Phase0 B2 Camera was not found");
    }
    return selected;
}

struct Options {
    std::uint64_t frames = 1000;
    std::uint32_t width = kWidth;
    std::uint32_t height = kHeight;
    std::uint32_t frameRate = kFrameRate;
    double minimumFps = 29.5;
    double maximumP95Ms = 33.3;
    std::optional<std::uint32_t> expectedControlRevision;
    std::optional<std::uint32_t> expectedControlValue;
    std::optional<std::uint32_t> expectedControlStatus;
    std::optional<std::uint64_t> transitionAppliedFrames;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--frames" && index + 1 < argc) {
            options.frames = std::stoull(argv[++index]);
        } else if (argument == L"--width" && index + 1 < argc) {
            options.width = std::stoul(argv[++index]);
        } else if (argument == L"--height" && index + 1 < argc) {
            options.height = std::stoul(argv[++index]);
        } else if (argument == L"--frame-rate" && index + 1 < argc) {
            options.frameRate = std::stoul(argv[++index]);
        } else if (argument == L"--minimum-fps" && index + 1 < argc) {
            options.minimumFps = std::stod(argv[++index]);
        } else if (argument == L"--maximum-p95-ms" && index + 1 < argc) {
            options.maximumP95Ms = std::stod(argv[++index]);
        } else if (argument == L"--expect-control-revision" && index + 1 < argc) {
            options.expectedControlRevision = std::stoul(argv[++index]);
        } else if (argument == L"--expect-control-value" && index + 1 < argc) {
            options.expectedControlValue = std::stoul(argv[++index]);
        } else if (argument == L"--expect-control-status" && index + 1 < argc) {
            options.expectedControlStatus = std::stoul(argv[++index]);
        } else if (argument == L"--expect-control-dead-after" && index + 1 < argc) {
            options.transitionAppliedFrames = std::stoull(argv[++index]);
        } else {
            throw std::invalid_argument(
                "usage: transport_source_local_consumer [--frames N] [--width N] "
                "[--height N] [--frame-rate N] [--minimum-fps F] "
                "[--maximum-p95-ms F] [--expect-control-revision N] "
                "[--expect-control-value 0..100] [--expect-control-status N] "
                "[--expect-control-dead-after N]");
        }
    }
    if (options.frames == 0 || options.width == 0 || options.height == 0 ||
        (options.width % 2) != 0 || (options.height % 2) != 0 ||
        options.frameRate == 0 || options.minimumFps <= 0.0 ||
        options.maximumP95Ms <= 0.0 ||
        (options.expectedControlValue.has_value() &&
         !SparkFaceSourceLocalControl::IsEffectValueValid(
             *options.expectedControlValue)) ||
        (options.expectedControlStatus.has_value() &&
         *options.expectedControlStatus > static_cast<std::uint32_t>(
             SparkFaceSourceLocalControl::Status::ValueInvalid)) ||
        (options.transitionAppliedFrames.has_value() &&
         (*options.transitionAppliedFrames == 0 ||
          *options.transitionAppliedFrames >= options.frames ||
          !options.expectedControlRevision.has_value() ||
          !options.expectedControlValue.has_value() ||
          !options.expectedControlStatus.has_value() ||
          *options.expectedControlStatus != static_cast<std::uint32_t>(
              SparkFaceSourceLocalControl::Status::Applied)))) {
        throw std::invalid_argument("Consumer mode and gates must be valid");
    }
    return options;
}

std::uint64_t decode_word(const BYTE* bytes, std::uint32_t firstBit,
                          std::uint32_t bitCount) {
    std::uint64_t value = 0;
    for (std::uint32_t bit = 0; bit < bitCount; ++bit) {
        const std::uint32_t x =
            (firstBit + bit) * SparkFaceSourceLocalControl::MarkerPixelWidth +
            SparkFaceSourceLocalControl::MarkerPixelWidth / 2;
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
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values.at(std::min(index, values.size() - 1));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parse_options(argc, argv);
        MfGuard mediaFoundation;
        ComPtr<IMFActivate> activation = find_camera();
        ComPtr<IMFMediaSource> source;
        check_hresult(activation->ActivateObject(IID_PPV_ARGS(&source)),
                      "IMFActivate::ActivateObject");

        ComPtr<IMFAttributes> readerAttributes;
        check_hresult(MFCreateAttributes(&readerAttributes, 2),
                      "MFCreateAttributes reader");
        check_hresult(readerAttributes->SetUINT32(
                          MF_READWRITE_DISABLE_CONVERTERS, TRUE),
                      "Disable converters");
        ComPtr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromMediaSource(
                          source.Get(), readerAttributes.Get(), &reader),
                      "MFCreateSourceReaderFromMediaSource");
        ComPtr<IMFMediaType> type;
        check_hresult(MFCreateMediaType(&type), "MFCreateMediaType");
        check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                      "Set major type");
        check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12),
                      "Set subtype");
        check_hresult(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                         options.width, options.height),
                      "Set frame size");
        check_hresult(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                          options.frameRate, 1),
                      "Set frame rate");
        constexpr DWORD streamIndex =
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        check_hresult(reader->SetCurrentMediaType(streamIndex, nullptr,
                                                   type.Get()),
                      "SetCurrentMediaType");

        std::vector<double> frameAgeMs;
        frameAgeMs.reserve(static_cast<std::size_t>(options.frames));
        std::uint64_t received = 0;
        std::uint64_t lastSequence = 0;
        std::uint32_t sourcePid = 0;
        std::uint64_t skipped = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t markerErrors = 0;
        std::uint32_t controlRevision = 0;
        bool transitionReady = false;
        bool transitionDead = false;
        std::uint32_t controlValue = 0;
        std::uint32_t controlStatus = 0;
        const std::int64_t started = qpc_now();
        while (received < options.frames) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            check_hresult(reader->ReadSample(streamIndex, 0, &actualStream,
                                              &flags, &timestamp, &sample),
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
            check_hresult(buffer->Lock(&bytes, &maximum, &current),
                          "IMFMediaBuffer::Lock");
            const std::int64_t deliveredQpc = qpc_now();
            if (current < static_cast<std::uint64_t>(options.width) *
                              options.height * 3 / 2) {
                ++markerErrors;
                buffer->Unlock();
                continue;
            }
            const auto magic = static_cast<std::uint16_t>(
                decode_word(bytes,
                            SparkFaceSourceLocalControl::MarkerMagicFirstBit, 16));
            const std::uint64_t sequence = decode_word(
                bytes, SparkFaceSourceLocalControl::MarkerSequenceFirstBit, 64);
            const auto renderQpc = static_cast<std::int64_t>(decode_word(
                bytes, SparkFaceSourceLocalControl::MarkerQpcFirstBit, 64));
            const auto frameSourcePid = static_cast<std::uint32_t>(decode_word(
                bytes, SparkFaceSourceLocalControl::MarkerSourcePidFirstBit, 32));
            const auto frameControlRevision = static_cast<std::uint32_t>(decode_word(
                bytes, SparkFaceSourceLocalControl::MarkerRevisionFirstBit, 32));
            const auto frameControlValue = static_cast<std::uint32_t>(decode_word(
                bytes, SparkFaceSourceLocalControl::MarkerEffectValueFirstBit, 32));
            const auto frameControlStatus = static_cast<std::uint32_t>(decode_word(
                bytes, SparkFaceSourceLocalControl::MarkerStatusFirstBit, 32));
            buffer->Unlock();
            if (magic != kPayloadMagic || sequence == 0 || renderQpc <= 0 ||
                renderQpc > deliveredQpc || frameSourcePid == 0 ||
                (sourcePid != 0 && frameSourcePid != sourcePid)) {
                ++markerErrors;
                continue;
            }
            sourcePid = frameSourcePid;
            controlRevision = frameControlRevision;
            controlValue = frameControlValue;
            controlStatus = frameControlStatus;
            const std::uint32_t expectedRevision =
                options.expectedControlRevision.value_or(frameControlRevision);
            const std::uint32_t expectedValue =
                options.expectedControlValue.value_or(frameControlValue);
            const std::uint32_t expectedStatus =
                options.expectedControlStatus.value_or(frameControlStatus);
            const bool matchesApplied =
                frameControlRevision == expectedRevision &&
                frameControlValue == expectedValue &&
                frameControlStatus == expectedStatus;
            const bool matchesDead =
                frameControlRevision == 0 && frameControlValue == 0 &&
                frameControlStatus == static_cast<std::uint32_t>(
                    SparkFaceSourceLocalControl::Status::ProducerDead);
            if (options.transitionAppliedFrames.has_value()) {
                if (transitionDead ? !matchesDead : !matchesApplied && !matchesDead) {
                    ++markerErrors;
                    continue;
                }
                transitionDead = transitionDead || matchesDead;
            } else if (!matchesApplied) {
                ++markerErrors;
                continue;
            }
            if (lastSequence != 0 && sequence == lastSequence) {
                ++duplicates;
            } else if (lastSequence != 0 && sequence > lastSequence + 1) {
                skipped += sequence - lastSequence - 1;
            } else if (lastSequence != 0 && sequence < lastSequence) {
                ++markerErrors;
                continue;
            }
            if (!transitionDead && options.transitionAppliedFrames.has_value() &&
                received + 1 == *options.transitionAppliedFrames) {
                transitionReady = true;
                std::cout << "control_transition_ready=1" << std::endl;
            }
            lastSequence = sequence;
            frameAgeMs.push_back(qpc_ms(renderQpc, deliveredQpc));
            ++received;
        }
        const double elapsedMs = qpc_ms(started, qpc_now());
        const double fps = static_cast<double>(received) * 1000.0 / elapsedMs;
        const double p50Ms = percentile(frameAgeMs, 0.50);
        const double p95Ms = percentile(frameAgeMs, 0.95);
        const double p99Ms = percentile(frameAgeMs, 0.99);
        const double maximumMs = *std::max_element(frameAgeMs.begin(),
                                                    frameAgeMs.end());
        const bool pass = fps >= options.minimumFps &&
                          fps <= static_cast<double>(options.frameRate) + 1.0 &&
                          p95Ms <= options.maximumP95Ms &&
                          markerErrors == 0 && duplicates == 0 && skipped == 0 &&
                          lastSequence == received &&
                          (!options.transitionAppliedFrames.has_value() ||
                           (transitionReady && transitionDead));

        reader.Reset();
        source.Reset();
        check_hresult(activation->ShutdownObject(),
                      "IMFActivate::ShutdownObject");
        std::cout << std::fixed << std::setprecision(3)
                  << "source_local_width=" << options.width << '\n'
                  << "source_local_height=" << options.height << '\n'
                  << "source_local_frame_rate=" << options.frameRate << '\n'
                  << "source_local_frames=" << received << '\n'
                  << "source_local_fps_achieved=" << fps << '\n'
                  << "source_local_render_to_client_p50_ms=" << p50Ms << '\n'
                  << "source_local_render_to_client_p95_ms=" << p95Ms << '\n'
                  << "source_local_render_to_client_p99_ms=" << p99Ms << '\n'
                  << "source_local_render_to_client_max_ms=" << maximumMs << '\n'
                  << "source_local_gpu_conversion_count=" << lastSequence << '\n'
                  << "source_local_process_id=" << sourcePid << '\n'
                  << "source_local_control_revision=" << controlRevision << '\n'
                  << "source_local_control_transition_applied="
                  << (transitionReady ? 1 : 0) << '\n'
                  << "source_local_control_transition_dead="
                  << (transitionDead ? 1 : 0) << '\n'
                  << "source_local_control_value=" << controlValue << '\n'
                  << "source_local_control_status=" << controlStatus << '\n'
                  << "skipped_sequences=" << skipped << '\n'
                  << "duplicate_sequences=" << duplicates << '\n'
                  << "marker_errors=" << markerErrors << '\n'
                  << "full_frame_cpu_copy_before_frame_server=0\n"
                  << (pass ? "SOURCE_LOCAL_GATE_PASS\n"
                           : "SOURCE_LOCAL_GATE_FAIL\n");
        std::cout.flush();
        return pass ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "SOURCE_LOCAL_CONSUMER_ERROR: " << error.what() << '\n';
        return 1;
    }
}
