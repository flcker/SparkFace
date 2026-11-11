#include "landmarks.h"
#include "preprocess.h"

// clang-format off
#include <Windows.h>
#include <Psapi.h>
// clang-format on
#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class Failure final : public std::runtime_error {
  public:
    explicit Failure(const char* const message) : std::runtime_error(message) {}
};

[[noreturn]] void fail(const char* const message) {
    throw Failure(message);
}

struct Config {
    std::filesystem::path model_path;
    std::filesystem::path image_path;
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    SfTensorLayout layout{};
    bool bgr{};
    std::array<float, 3> scale{};
    std::array<float, 3> bias{};
    std::size_t landmark_count{};
    std::size_t dimensions{};
    SfCoordinateMode coordinate_mode{};
    std::size_t warmup_count{10};
    std::size_t frame_count{100};
    std::optional<std::filesystem::path> overlay_path;
    std::size_t input_index{};
    std::size_t output_index{};
    double max_p95_ms{8.0};
};

struct RequiredOptions {
    bool model{};
    bool image{};
    bool input_width{};
    bool input_height{};
    bool layout{};
    bool channel_order{};
    bool scale{};
    bool bias{};
    bool landmark_count{};
    bool dimensions{};
    bool coordinate_mode{};
};

std::string_view require_value(const int argc, char* argv[], int& index, const char* const option) {
    if (index + 1 >= argc) {
        fail(option);
    }
    ++index;
    return argv[index];
}

std::size_t parse_size(const std::string_view value, const char* const error) {
    if (value.empty() || value.front() == '-') {
        fail(error);
    }

    char* end                       = nullptr;
    errno                           = 0;
    const unsigned long long parsed = std::strtoull(value.data(), &end, 10);
    if (errno == ERANGE || end != value.data() + value.size() ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        fail(error);
    }
    return static_cast<std::size_t>(parsed);
}

std::uint32_t parse_u32(const std::string_view value, const char* const error) {
    const std::size_t parsed = parse_size(value, error);
    if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
        fail(error);
    }
    return static_cast<std::uint32_t>(parsed);
}

float parse_float(const std::string_view value, const char* const error) {
    std::string text(value);
    char* end          = nullptr;
    errno              = 0;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(parsed)) {
        fail(error);
    }
    return parsed;
}

std::array<float, 3> parse_channel_values(const std::string_view value, const char* const error) {
    if (value.find(',') == std::string_view::npos) {
        const float scalar = parse_float(value, error);
        return {scalar, scalar, scalar};
    }

    std::array<float, 3> values{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::size_t end = value.find(',', start);
        if ((index < values.size() - 1 && end == std::string_view::npos) ||
            (index == values.size() - 1 && end != std::string_view::npos)) {
            fail(error);
        }
        const std::size_t length =
            end == std::string_view::npos ? value.size() - start : end - start;
        values[index] = parse_float(value.substr(start, length), error);
        start         = end == std::string_view::npos ? value.size() : end + 1;
    }
    return values;
}

double parse_double(const std::string_view value, const char* const error) {
    std::string text(value);
    char* end           = nullptr;
    errno               = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(parsed)) {
        fail(error);
    }
    return parsed;
}

SfTensorLayout parse_layout(const std::string_view value) {
    if (value == "nchw") {
        return SfTensorLayout::nchw;
    }
    if (value == "nhwc") {
        return SfTensorLayout::nhwc;
    }
    fail("invalid_layout");
}

bool parse_channel_order(const std::string_view value) {
    if (value == "rgb") {
        return false;
    }
    if (value == "bgr") {
        return true;
    }
    fail("invalid_channel_order");
}

SfCoordinateMode parse_coordinate_mode(const std::string_view value) {
    if (value == "normalized-zero-one") {
        return SfCoordinateMode::normalized_zero_one;
    }
    if (value == "normalized-minus-one-one") {
        return SfCoordinateMode::normalized_minus_one_one;
    }
    if (value == "input-pixels") {
        return SfCoordinateMode::input_pixels;
    }
    fail("invalid_coordinate_mode");
}

const char* layout_name(const SfTensorLayout layout) {
    return layout == SfTensorLayout::nchw ? "nchw" : "nhwc";
}

const char* channel_order_name(const bool bgr) {
    return bgr ? "bgr" : "rgb";
}

const char* coordinate_mode_name(const SfCoordinateMode mode) {
    switch (mode) {
    case SfCoordinateMode::normalized_zero_one:
        return "normalized-zero-one";
    case SfCoordinateMode::normalized_minus_one_one:
        return "normalized-minus-one-one";
    case SfCoordinateMode::input_pixels:
        return "input-pixels";
    }
    fail("invalid_coordinate_mode");
}

void print_usage() {
    std::cout << "usage: face_spike --model PATH --image PATH --input-width N --input-height N "
                 "--layout nchw|nhwc --channel-order rgb|bgr --scale F|F,F,F --bias F|F,F,F "
                 "--landmark-count N --dimensions N --coordinate-mode "
                 "normalized-zero-one|normalized-minus-one-one|input-pixels [options]\n"
                 "options: --warmup N (default 10) --frames N (default 100) --max-p95-ms F "
                 "(default 8) --overlay PATH --input-index N --output-index N\n";
}

Config parse_arguments(const int argc, char* argv[]) {
    Config config;
    RequiredOptions required;

    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") {
            print_usage();
            std::exit(0);
        }
        if (option == "--model") {
            config.model_path = std::filesystem::path(
                std::string(require_value(argc, argv, index, "missing_model")));
            required.model = true;
        } else if (option == "--image") {
            config.image_path = std::filesystem::path(
                std::string(require_value(argc, argv, index, "missing_image")));
            required.image = true;
        } else if (option == "--input-width") {
            config.input_width = parse_u32(require_value(argc, argv, index, "invalid_input_width"),
                                           "invalid_input_width");
            required.input_width = true;
        } else if (option == "--input-height") {
            config.input_height = parse_u32(
                require_value(argc, argv, index, "invalid_input_height"), "invalid_input_height");
            required.input_height = true;
        } else if (option == "--layout") {
            config.layout   = parse_layout(require_value(argc, argv, index, "missing_layout"));
            required.layout = true;
        } else if (option == "--channel-order") {
            config.bgr =
                parse_channel_order(require_value(argc, argv, index, "missing_channel_order"));
            required.channel_order = true;
        } else if (option == "--rgb") {
            config.bgr             = false;
            required.channel_order = true;
        } else if (option == "--bgr") {
            config.bgr             = true;
            required.channel_order = true;
        } else if (option == "--scale") {
            config.scale   = parse_channel_values(require_value(argc, argv, index, "invalid_scale"),
                                                  "invalid_scale");
            required.scale = true;
        } else if (option == "--bias") {
            config.bias   = parse_channel_values(require_value(argc, argv, index, "invalid_bias"),
                                                 "invalid_bias");
            required.bias = true;
        } else if (option == "--landmark-count") {
            config.landmark_count =
                parse_size(require_value(argc, argv, index, "invalid_landmark_count"),
                           "invalid_landmark_count");
            if (config.landmark_count == 0) {
                fail("invalid_landmark_count");
            }
            required.landmark_count = true;
        } else if (option == "--dimensions") {
            config.dimensions = parse_size(require_value(argc, argv, index, "invalid_dimensions"),
                                           "invalid_dimensions");
            if (config.dimensions == 0) {
                fail("invalid_dimensions");
            }
            required.dimensions = true;
        } else if (option == "--coordinate-mode") {
            config.coordinate_mode =
                parse_coordinate_mode(require_value(argc, argv, index, "missing_coordinate_mode"));
            required.coordinate_mode = true;
        } else if (option == "--warmup") {
            config.warmup_count =
                parse_size(require_value(argc, argv, index, "invalid_warmup"), "invalid_warmup");
        } else if (option == "--frames") {
            config.frame_count =
                parse_size(require_value(argc, argv, index, "invalid_frames"), "invalid_frames");
            if (config.frame_count == 0) {
                fail("invalid_frames");
            }
        } else if (option == "--max-p95-ms") {
            config.max_p95_ms = parse_double(require_value(argc, argv, index, "invalid_max_p95_ms"),
                                             "invalid_max_p95_ms");
            if (config.max_p95_ms < 0.0) {
                fail("invalid_max_p95_ms");
            }
        } else if (option == "--overlay") {
            config.overlay_path = std::filesystem::path(
                std::string(require_value(argc, argv, index, "missing_overlay_path")));
        } else if (option == "--input-index") {
            config.input_index = parse_size(require_value(argc, argv, index, "invalid_input_index"),
                                            "invalid_input_index");
        } else if (option == "--output-index") {
            config.output_index = parse_size(
                require_value(argc, argv, index, "invalid_output_index"), "invalid_output_index");
        } else {
            fail("unknown_option");
        }
    }

    if (!required.model || !required.image || !required.input_width || !required.input_height ||
        !required.layout || !required.channel_order || !required.scale || !required.bias ||
        !required.landmark_count || !required.dimensions || !required.coordinate_mode) {
        fail("missing_required_option");
    }
    return config;
}

std::size_t checked_product(const std::vector<std::int64_t>& shape, const char* const error) {
    std::size_t count = 1;
    for (const std::int64_t dimension : shape) {
        if (dimension <= 0 || static_cast<std::uint64_t>(dimension) >
                                  std::numeric_limits<std::size_t>::max() / count) {
            fail(error);
        }
        count *= static_cast<std::size_t>(dimension);
    }
    return count;
}

std::size_t checked_product(const std::size_t first, const std::size_t second,
                            const char* const error) {
    if (first == 0 || second == 0 || first > std::numeric_limits<std::size_t>::max() / second) {
        fail(error);
    }
    return first * second;
}

std::string shape_string(const std::vector<std::int64_t>& shape) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index != 0) {
            stream << 'x';
        }
        stream << shape[index];
    }
    return stream.str();
}

void print_model_io_metadata(Ort::Session& session) {
    Ort::AllocatorWithDefaultOptions allocator;
    std::cout << "provider=DmlExecutionProvider\n"
              << "ort_dml_version=" << SF_ORT_DML_VERSION << '\n'
              << "model_input_count=" << session.GetInputCount() << '\n'
              << "model_output_count=" << session.GetOutputCount() << '\n';

    for (std::size_t index = 0; index < session.GetInputCount(); ++index) {
        const auto info = session.GetInputTypeInfo(index).GetTensorTypeAndShapeInfo();
        const auto name = session.GetInputNameAllocated(index, allocator);
        std::cout << "model_input_" << index << "_name=" << name.get() << '\n'
                  << "model_input_" << index << "_type=" << static_cast<int>(info.GetElementType())
                  << '\n'
                  << "model_input_" << index << "_shape=" << shape_string(info.GetShape()) << '\n';
    }
    for (std::size_t index = 0; index < session.GetOutputCount(); ++index) {
        const auto info = session.GetOutputTypeInfo(index).GetTensorTypeAndShapeInfo();
        const auto name = session.GetOutputNameAllocated(index, allocator);
        std::cout << "model_output_" << index << "_name=" << name.get() << '\n'
                  << "model_output_" << index << "_type=" << static_cast<int>(info.GetElementType())
                  << '\n'
                  << "model_output_" << index << "_shape=" << shape_string(info.GetShape()) << '\n';
    }
}

std::vector<std::int64_t> expected_input_shape(const Config& config) {
    if (config.layout == SfTensorLayout::nchw) {
        return {1, 3, static_cast<std::int64_t>(config.input_height),
                static_cast<std::int64_t>(config.input_width)};
    }
    return {1, static_cast<std::int64_t>(config.input_height),
            static_cast<std::int64_t>(config.input_width), 3};
}

void validate_input_shape(const std::vector<std::int64_t>& model_shape,
                          const std::vector<std::int64_t>& requested_shape) {
    if (model_shape.size() != requested_shape.size()) {
        fail("input_rank_mismatch");
    }
    for (std::size_t index = 0; index < model_shape.size(); ++index) {
        if (model_shape[index] > 0 && model_shape[index] != requested_shape[index]) {
            fail("input_shape_mismatch");
        }
    }
}

std::vector<std::int64_t> concrete_output_shape(std::vector<std::int64_t> model_shape) {
    for (std::size_t index = 0; index < model_shape.size(); ++index) {
        if (model_shape[index] <= 0) {
            if (index != 0) {
                fail("output_shape_not_static");
            }
            model_shape[index] = 1;
        }
    }
    return model_shape;
}

bool values_are_finite(const std::vector<float>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](const float value) { return std::isfinite(value); });
}

std::int64_t qpc_now() {
    LARGE_INTEGER count{};
    if (QueryPerformanceCounter(&count) == FALSE) {
        fail("qpc_query_failed");
    }
    return count.QuadPart;
}

double qpc_frequency() {
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) {
        fail("qpc_frequency_failed");
    }
    return static_cast<double>(frequency.QuadPart);
}

double percentile_ms(std::vector<double> samples, const std::size_t numerator,
                     const std::size_t denominator) {
    if (samples.empty()) {
        fail("no_measured_frames");
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t rank = (samples.size() * numerator + denominator - 1) / denominator;
    return samples[rank - 1];
}

std::size_t peak_working_set_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             static_cast<DWORD>(sizeof(counters))) == FALSE) {
        fail("working_set_query_failed");
    }
    return counters.PeakWorkingSetSize;
}

void print_profile(const Config& config) {
    std::cout << "model_path=" << config.model_path.generic_string() << '\n'
              << "image_path=" << config.image_path.generic_string() << '\n'
              << "profile_input_width=" << config.input_width << '\n'
              << "profile_input_height=" << config.input_height << '\n'
              << "profile_layout=" << layout_name(config.layout) << '\n'
              << "profile_channel_order=" << channel_order_name(config.bgr) << '\n'
              << "profile_scale=" << config.scale[0] << ',' << config.scale[1] << ','
              << config.scale[2] << '\n'
              << "profile_bias=" << config.bias[0] << ',' << config.bias[1] << ',' << config.bias[2]
              << '\n'
              << "profile_landmark_count=" << config.landmark_count << '\n'
              << "profile_dimensions=" << config.dimensions << '\n'
              << "profile_coordinate_mode=" << coordinate_mode_name(config.coordinate_mode) << '\n'
              << "profile_input_index=" << config.input_index << '\n'
              << "profile_output_index=" << config.output_index << '\n'
              << "warmup_frames=" << config.warmup_count << '\n'
              << "measured_frames_requested=" << config.frame_count << '\n'
              << "max_p95_ms=" << config.max_p95_ms << '\n';
}

int run(const Config& config) {
    if (!std::filesystem::is_regular_file(config.model_path)) {
        fail("model_not_found");
    }
    if (!std::filesystem::is_regular_file(config.image_path)) {
        fail("image_not_found");
    }
    const std::vector<std::string> providers = Ort::GetAvailableProviders();
    if (std::find(providers.begin(), providers.end(), "DmlExecutionProvider") == providers.end()) {
        fail("directml_provider_unavailable");
    }

    SfImage image;
    try {
        image = sf_load_ppm(config.image_path);
    } catch (const std::exception&) {
        fail("image_load_failed");
    }

    const SfPreprocessSpec preprocess_spec{config.input_width, config.input_height, config.layout,
                                           config.bgr,         config.scale,        config.bias};
    std::vector<float> input_data;
    try {
        input_data = sf_preprocess(image, preprocess_spec);
    } catch (const std::exception&) {
        fail("preprocess_failed");
    }

    const std::vector<std::int64_t> input_shape = expected_input_shape(config);
    const std::size_t expected_input_elements =
        checked_product(input_shape, "input_shape_overflow");
    if (input_data.size() != expected_input_elements) {
        fail("preprocess_size_mismatch");
    }

    Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "sparkface-face-spike");
    Ort::SessionOptions options;
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.DisableMemPattern();
    options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    try {
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, 0));
    } catch (const Ort::Exception&) {
        fail("directml_provider_setup_failed");
    }

    Ort::Session session{nullptr};
    try {
        session = Ort::Session(environment, config.model_path.c_str(), options);
    } catch (const Ort::Exception&) {
        fail("session_creation_failed");
    }

    try {
        print_model_io_metadata(session);
    } catch (const Ort::Exception&) {
        fail("model_metadata_failed");
    }

    if (config.input_index >= session.GetInputCount() ||
        config.output_index >= session.GetOutputCount()) {
        fail("node_index_out_of_range");
    }

    Ort::AllocatorWithDefaultOptions allocator;
    std::string input_name_storage;
    std::string output_name_storage;
    std::vector<std::int64_t> output_shape;
    try {
        const auto input_info =
            session.GetInputTypeInfo(config.input_index).GetTensorTypeAndShapeInfo();
        if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            fail("input_type_not_float");
        }
        validate_input_shape(input_info.GetShape(), input_shape);

        const auto output_info =
            session.GetOutputTypeInfo(config.output_index).GetTensorTypeAndShapeInfo();
        if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            fail("output_type_not_float");
        }
        output_shape = concrete_output_shape(output_info.GetShape());
        const auto allocated_input_name =
            session.GetInputNameAllocated(config.input_index, allocator);
        const auto allocated_output_name =
            session.GetOutputNameAllocated(config.output_index, allocator);
        input_name_storage  = allocated_input_name.get();
        output_name_storage = allocated_output_name.get();
    } catch (const Ort::Exception&) {
        fail("model_metadata_failed");
    }

    const std::size_t expected_output_elements =
        checked_product(config.landmark_count, config.dimensions, "landmark_shape_overflow");
    const std::size_t output_elements = checked_product(output_shape, "output_shape_not_static");
    if (output_elements != expected_output_elements) {
        fail("output_length_mismatch");
    }

    std::vector<float> output_data(output_elements);
    const Ort::MemoryInfo memory_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, input_data.data(), input_data.size(), input_shape.data(), input_shape.size());
    Ort::Value output_tensor =
        Ort::Value::CreateTensor<float>(memory_info, output_data.data(), output_data.size(),
                                        output_shape.data(), output_shape.size());
    const char* const input_name  = input_name_storage.c_str();
    const char* const output_name = output_name_storage.c_str();
    Ort::RunOptions run_options{nullptr};

    for (std::size_t frame = 0; frame < config.warmup_count; ++frame) {
        try {
            session.Run(run_options, &input_name, &input_tensor, 1, &output_name, &output_tensor,
                        1);
        } catch (const Ort::Exception&) {
            fail("inference_failed");
        }
        if (!values_are_finite(output_data)) {
            fail("non_finite_warmup_output");
        }
    }

    const double frequency = qpc_frequency();
    std::vector<double> frame_times_ms;
    frame_times_ms.reserve(config.frame_count);
    bool finite_outputs = true;
    for (std::size_t frame = 0; frame < config.frame_count; ++frame) {
        const std::int64_t start = qpc_now();
        try {
            session.Run(run_options, &input_name, &input_tensor, 1, &output_name, &output_tensor,
                        1);
        } catch (const Ort::Exception&) {
            fail("inference_failed");
        }
        const std::int64_t stop = qpc_now();
        if (stop < start) {
            fail("qpc_clock_invalid");
        }
        frame_times_ms.push_back(static_cast<double>(stop - start) * 1000.0 / frequency);
        finite_outputs = finite_outputs && values_are_finite(output_data);
    }

    const double p50_ms = percentile_ms(frame_times_ms, 50, 100);
    const double p95_ms = percentile_ms(frame_times_ms, 95, 100);
    const double p99_ms = percentile_ms(frame_times_ms, 99, 100);
    const double max_ms = *std::max_element(frame_times_ms.begin(), frame_times_ms.end());
    const std::size_t peak_working_set = peak_working_set_bytes();

    std::vector<SfLandmark> landmarks;
    try {
        landmarks = sf_decode_landmarks(
            output_data.data(), output_data.size(), config.landmark_count, config.dimensions,
            config.coordinate_mode, config.input_width, config.input_height);
    } catch (const std::exception&) {
        fail("landmark_decode_failed");
    }
    if (landmarks.size() != config.landmark_count) {
        fail("landmark_count_mismatch");
    }
    if (config.overlay_path.has_value()) {
        try {
            sf_write_overlay_ppm(image, landmarks, *config.overlay_path);
        } catch (const std::exception&) {
            fail("overlay_write_failed");
        }
    }

    print_profile(config);
    std::cout << std::fixed << std::setprecision(6) << "p50_ms=" << p50_ms << '\n'
              << "p95_ms=" << p95_ms << '\n'
              << "p99_ms=" << p99_ms << '\n'
              << "max_ms=" << max_ms << '\n'
              << "working_set_peak_bytes=" << peak_working_set << '\n'
              << "measured_frames_completed=" << frame_times_ms.size() << '\n'
              << "finite_outputs=" << (finite_outputs ? 1 : 0) << '\n'
              << "p95_gate_pass=" << (p95_ms <= config.max_p95_ms ? 1 : 0) << '\n';

    const bool pass = finite_outputs && frame_times_ms.size() == config.frame_count &&
                      p95_ms <= config.max_p95_ms;
    std::cout << (pass ? "FACE_SPIKE_PASS\n" : "FACE_SPIKE_FAIL\n");
    return pass ? 0 : 2;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const Failure& error) {
        std::cerr << "FACE_SPIKE_ERROR: " << error.what() << '\n';
    } catch (const Ort::Exception&) {
        std::cerr << "FACE_SPIKE_ERROR: onnx_runtime_failure\n";
    } catch (const std::exception&) {
        std::cerr << "FACE_SPIKE_ERROR: harness_failure\n";
    }
    return 1;
}
