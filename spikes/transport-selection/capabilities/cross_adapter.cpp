// SPDX-License-Identifier: Apache-2.0
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT64 kBufferBytes = 64U * 1024U;
constexpr UINT kTextureWidth = 1920;
constexpr UINT kTextureHeight = 1080;
constexpr DWORD kWaitMilliseconds = 10'000;

class HResultError final : public std::runtime_error {
  public:
    HResultError(HRESULT result, const std::string& operation)
        : std::runtime_error(operation), result_(result) {}

    [[nodiscard]] HRESULT result() const { return result_; }

  private:
    HRESULT result_;
};

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw HResultError(result, operation);
    }
}

class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

    [[nodiscard]] HANDLE get() const { return value_; }

  private:
    HANDLE value_ = nullptr;
};

std::string hresult_string(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

std::string luid_string(const LUID& luid) {
    std::ostringstream stream;
    stream << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(luid.HighPart) << ':' << std::setw(8)
           << luid.LowPart;
    return stream.str();
}

bool luid_equal(const LUID& left, const LUID& right) {
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

LUID parse_luid(const std::string& value) {
    const auto separator = value.find(':');
    if (separator != 8 || value.size() != 17) {
        throw std::invalid_argument("LUID must use eight hexadecimal digits per half");
    }
    const auto high = static_cast<std::uint32_t>(std::stoul(value.substr(0, 8), nullptr, 16));
    const auto low = static_cast<std::uint32_t>(std::stoul(value.substr(9, 8), nullptr, 16));
    return LUID{low, static_cast<LONG>(high)};
}

std::string narrow(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) {
        return {};
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), count, nullptr, nullptr);
    output.pop_back();
    return output;
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_description(UINT64 bytes,
                                       D3D12_RESOURCE_FLAGS flags =
                                           D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_DESC texture_description(DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = kTextureWidth;
    description.Height = kTextureHeight;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    return description;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type,
                                     UINT64 bytes, D3D12_RESOURCE_STATES initial_state) {
    ComPtr<ID3D12Resource> resource;
    const auto properties = heap_properties(heap_type);
    const auto description = buffer_description(bytes);
    check_hresult(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                                                  &description, initial_state, nullptr,
                                                  IID_PPV_ARGS(&resource)),
                  "CreateCommittedResource buffer");
    return resource;
}

ComPtr<ID3D12Resource> create_cross_adapter_resource(ID3D12Device* device,
                                                     const D3D12_RESOURCE_DESC& description) {
    ComPtr<ID3D12Resource> resource;
    const auto properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    constexpr auto heap_flags = static_cast<D3D12_HEAP_FLAGS>(
        D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
    check_hresult(device->CreateCommittedResource(&properties, heap_flags, &description,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&resource)),
                  "CreateCommittedResource cross-adapter");
    return resource;
}

ComPtr<ID3D12CommandQueue> create_queue(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    description.NodeMask = 0;
    ComPtr<ID3D12CommandQueue> queue;
    check_hresult(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)),
                  "CreateCommandQueue");
    return queue;
}

struct CommandContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

CommandContext create_command_context(ID3D12Device* device) {
    CommandContext context;
    check_hresult(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&context.allocator)),
                  "CreateCommandAllocator");
    check_hresult(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            context.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&context.list)),
                  "CreateCommandList");
    return context;
}

UniqueHandle share_object(ID3D12Device* device, ID3D12DeviceChild* object) {
    HANDLE value = nullptr;
    check_hresult(device->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &value),
                  "CreateSharedHandle");
    return UniqueHandle(value);
}

void execute(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
    check_hresult(list->Close(), "Close command list");
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
}

void wait_for_fence(ID3D12Fence* fence, UINT64 value) {
    if (fence->GetCompletedValue() >= value) {
        return;
    }
    UniqueHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.get() == nullptr) {
        throw std::runtime_error("CreateEvent failed");
    }
    check_hresult(fence->SetEventOnCompletion(value, event.get()), "SetEventOnCompletion");
    const DWORD wait = WaitForSingleObject(event.get(), kWaitMilliseconds);
    if (wait != WAIT_OBJECT_0) {
        throw std::runtime_error("GPU fence wait exceeded 10 seconds");
    }
}

struct SharedFence {
    ComPtr<ID3D12Fence> producer;
    ComPtr<ID3D12Fence> consumer;
};

SharedFence create_shared_fence(ID3D12Device* producer_device,
                                ID3D12Device* consumer_device) {
    SharedFence result;
    constexpr auto flags = static_cast<D3D12_FENCE_FLAGS>(
        D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER);
    check_hresult(producer_device->CreateFence(0, flags, IID_PPV_ARGS(&result.producer)),
                  "CreateFence cross-adapter");
    const auto handle = share_object(producer_device, result.producer.Get());
    check_hresult(consumer_device->OpenSharedHandle(handle.get(),
                                                    IID_PPV_ARGS(&result.consumer)),
                  "OpenSharedHandle fence");
    return result;
}

ComPtr<ID3D12Resource> open_resource(ID3D12Device* producer_device,
                                     ID3D12Device* consumer_device,
                                     ID3D12Resource* producer_resource) {
    const auto handle = share_object(producer_device, producer_resource);
    ComPtr<ID3D12Resource> consumer_resource;
    check_hresult(consumer_device->OpenSharedHandle(handle.get(),
                                                    IID_PPV_ARGS(&consumer_resource)),
                  "OpenSharedHandle resource");
    return consumer_resource;
}

void synchronize_copy(ID3D12CommandQueue* producer_queue,
                      ID3D12CommandQueue* consumer_queue, SharedFence& fence,
                      ID3D12GraphicsCommandList* producer_list,
                      ID3D12GraphicsCommandList* consumer_list) {
    execute(producer_queue, producer_list);
    check_hresult(producer_queue->Signal(fence.producer.Get(), 1),
                  "Signal producer cross-adapter fence");
    check_hresult(consumer_queue->Wait(fence.consumer.Get(), 1),
                  "Wait consumer cross-adapter fence");
    execute(consumer_queue, consumer_list);
    check_hresult(consumer_queue->Signal(fence.consumer.Get(), 2),
                  "Signal consumer cross-adapter fence");
    wait_for_fence(fence.producer.Get(), 2);
}

void test_buffer_copy(ID3D12Device* producer_device, ID3D12Device* consumer_device) {
    const auto description =
        buffer_description(kBufferBytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    auto producer_shared = create_cross_adapter_resource(producer_device, description);
    auto consumer_shared =
        open_resource(producer_device, consumer_device, producer_shared.Get());
    auto upload = create_buffer(producer_device, D3D12_HEAP_TYPE_UPLOAD, kBufferBytes,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = create_buffer(consumer_device, D3D12_HEAP_TYPE_READBACK, kBufferBytes,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

    std::byte* upload_bytes = nullptr;
    D3D12_RANGE no_read{0, 0};
    check_hresult(upload->Map(0, &no_read, reinterpret_cast<void**>(&upload_bytes)),
                  "Map upload buffer");
    for (UINT64 index = 0; index < kBufferBytes; ++index) {
        upload_bytes[index] = static_cast<std::byte>((index * 37U + 11U) & 0xffU);
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(kBufferBytes)};
    upload->Unmap(0, &written);

    auto producer_queue = create_queue(producer_device);
    auto consumer_queue = create_queue(consumer_device);
    auto producer_commands = create_command_context(producer_device);
    auto consumer_commands = create_command_context(consumer_device);
    auto fence = create_shared_fence(producer_device, consumer_device);

    auto barrier = transition(producer_shared.Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    producer_commands.list->ResourceBarrier(1, &barrier);
    producer_commands.list->CopyBufferRegion(producer_shared.Get(), 0, upload.Get(), 0,
                                              kBufferBytes);
    barrier = transition(producer_shared.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_COMMON);
    producer_commands.list->ResourceBarrier(1, &barrier);

    barrier = transition(consumer_shared.Get(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    consumer_commands.list->ResourceBarrier(1, &barrier);
    consumer_commands.list->CopyBufferRegion(readback.Get(), 0, consumer_shared.Get(), 0,
                                              kBufferBytes);
    barrier = transition(consumer_shared.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
    consumer_commands.list->ResourceBarrier(1, &barrier);

    synchronize_copy(producer_queue.Get(), consumer_queue.Get(), fence,
                     producer_commands.list.Get(), consumer_commands.list.Get());

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(kBufferBytes)};
    check_hresult(readback->Map(0, &read_range, &mapped), "Map readback buffer");
    const auto* result = static_cast<const std::byte*>(mapped);
    bool matches = true;
    for (UINT64 index = 0; index < kBufferBytes; ++index) {
        const auto expected = static_cast<std::byte>((index * 37U + 11U) & 0xffU);
        if (result[index] != expected) {
            matches = false;
            break;
        }
    }
    D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    if (!matches) {
        throw std::runtime_error("cross-adapter buffer payload mismatch");
    }
}

void test_r8_texture_copy(ID3D12Device* producer_device, ID3D12Device* consumer_device) {
    const auto description = texture_description(DXGI_FORMAT_R8_UNORM);
    auto producer_shared = create_cross_adapter_resource(producer_device, description);
    auto consumer_shared =
        open_resource(producer_device, consumer_device, producer_shared.Get());

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    producer_device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows,
                                           &row_bytes, &total_bytes);
    if (rows != kTextureHeight || row_bytes != kTextureWidth || total_bytes == 0) {
        throw std::runtime_error("unexpected R8 copy footprint");
    }

    auto upload = create_buffer(producer_device, D3D12_HEAP_TYPE_UPLOAD, total_bytes,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = create_buffer(consumer_device, D3D12_HEAP_TYPE_READBACK, total_bytes,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

    std::byte* upload_bytes = nullptr;
    D3D12_RANGE no_read{0, 0};
    check_hresult(upload->Map(0, &no_read, reinterpret_cast<void**>(&upload_bytes)),
                  "Map texture upload buffer");
    std::fill(upload_bytes, upload_bytes + total_bytes, std::byte{0});
    for (UINT y = 0; y < kTextureHeight; ++y) {
        auto* row = upload_bytes + footprint.Offset +
                    static_cast<UINT64>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kTextureWidth; ++x) {
            row[x] = static_cast<std::byte>((x * 13U + y * 17U + 5U) & 0xffU);
        }
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(total_bytes)};
    upload->Unmap(0, &written);

    auto producer_queue = create_queue(producer_device);
    auto consumer_queue = create_queue(consumer_device);
    auto producer_commands = create_command_context(producer_device);
    auto consumer_commands = create_command_context(consumer_device);
    auto fence = create_shared_fence(producer_device, consumer_device);

    D3D12_TEXTURE_COPY_LOCATION upload_location{};
    upload_location.pResource = upload.Get();
    upload_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    upload_location.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION producer_location{};
    producer_location.pResource = producer_shared.Get();
    producer_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    producer_location.SubresourceIndex = 0;

    auto barrier = transition(producer_shared.Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    producer_commands.list->ResourceBarrier(1, &barrier);
    producer_commands.list->CopyTextureRegion(&producer_location, 0, 0, 0,
                                              &upload_location, nullptr);
    barrier = transition(producer_shared.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_COMMON);
    producer_commands.list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION consumer_location{};
    consumer_location.pResource = consumer_shared.Get();
    consumer_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    consumer_location.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION readback_location{};
    readback_location.pResource = readback.Get();
    readback_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readback_location.PlacedFootprint = footprint;

    barrier = transition(consumer_shared.Get(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    consumer_commands.list->ResourceBarrier(1, &barrier);
    consumer_commands.list->CopyTextureRegion(&readback_location, 0, 0, 0,
                                              &consumer_location, nullptr);
    barrier = transition(consumer_shared.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
    consumer_commands.list->ResourceBarrier(1, &barrier);

    synchronize_copy(producer_queue.Get(), consumer_queue.Get(), fence,
                     producer_commands.list.Get(), consumer_commands.list.Get());

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    check_hresult(readback->Map(0, &read_range, &mapped),
                  "Map texture readback buffer");
    const auto* result = static_cast<const std::byte*>(mapped);
    bool matches = true;
    for (UINT y = 0; y < kTextureHeight && matches; ++y) {
        const auto* row = result + footprint.Offset +
                          static_cast<UINT64>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kTextureWidth; ++x) {
            const auto expected =
                static_cast<std::byte>((x * 13U + y * 17U + 5U) & 0xffU);
            if (row[x] != expected) {
                matches = false;
                break;
            }
        }
    }
    D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    if (!matches) {
        throw std::runtime_error("cross-adapter R8 texture payload mismatch");
    }
}

void test_nv12_texture_open(ID3D12Device* producer_device, ID3D12Device* consumer_device) {
    const auto description = texture_description(DXGI_FORMAT_NV12);
    auto producer_resource = create_cross_adapter_resource(producer_device, description);
    auto consumer_resource =
        open_resource(producer_device, consumer_device, producer_resource.Get());
    if (consumer_resource->GetDesc().Format != DXGI_FORMAT_NV12) {
        throw std::runtime_error("opened NV12 resource changed format");
    }
}

template <typename Function>
void run_case(const char* name, Function&& function) {
    try {
        function();
        std::cout << name << "_status=pass\n";
    } catch (const HResultError& error) {
        std::cout << name << "_status=fail\n";
        std::cout << name << "_hresult=" << hresult_string(error.result()) << '\n';
        std::cout << name << "_operation=" << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cout << name << "_status=fail\n";
        std::cout << name << "_error=" << error.what() << '\n';
    }
}

struct AdapterRecord {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
};

std::vector<AdapterRecord> enumerate_adapters(IDXGIFactory6* factory) {
    std::vector<AdapterRecord> adapters;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check_hresult(result, "EnumAdapters1");
        AdapterRecord record;
        record.adapter = std::move(adapter);
        check_hresult(record.adapter->GetDesc1(&record.description), "GetDesc1");
        adapters.push_back(std::move(record));
    }
    return adapters;
}

AdapterRecord* choose_producer(std::vector<AdapterRecord>& adapters) {
    auto hardware = [](const AdapterRecord& adapter) {
        return (adapter.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
    };
    auto is_intel = [](const AdapterRecord& adapter) {
        const std::wstring name(adapter.description.Description);
        return name.find(L"Intel") != std::wstring::npos;
    };
    auto selected = std::find_if(adapters.begin(), adapters.end(),
                                 [&](const AdapterRecord& adapter) {
                                     return hardware(adapter) && is_intel(adapter);
                                 });
    if (selected == adapters.end()) {
        selected = std::find_if(adapters.begin(), adapters.end(), hardware);
    }
    return selected == adapters.end() ? nullptr : &*selected;
}

void print_cross_node_support(const char* prefix, ID3D12Device* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const HRESULT result =
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (FAILED(result)) {
        std::cout << prefix << "_cross_node_query=fail\n";
        std::cout << prefix << "_cross_node_query_hresult=" << hresult_string(result) << '\n';
        return;
    }
    std::cout << prefix << "_cross_node_query=pass\n";
    std::cout << prefix << "_cross_node_sharing_tier="
              << static_cast<unsigned>(options.CrossNodeSharingTier) << '\n';
    std::cout << prefix << "_cross_node_supported="
              << (options.CrossNodeSharingTier == D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED
                      ? "false"
                      : "true")
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string consumer_luid_text = "00000000:000178d8";
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--consumer-luid" && index + 1 < argc) {
                consumer_luid_text = argv[++index];
            } else {
                throw std::invalid_argument("usage: transport_cross_adapter_probe --consumer-luid HHHHHHHH:LLLLLLLL");
            }
        }
        const LUID consumer_luid = parse_luid(consumer_luid_text);

        std::cout << "transport_selection_schema=1\n";
        std::cout << "requested_consumer_luid=" << luid_string(consumer_luid) << '\n';

        ComPtr<IDXGIFactory6> factory;
        check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        auto adapters = enumerate_adapters(factory.Get());
        std::cout << "adapter_count=" << adapters.size() << '\n';
        for (std::size_t index = 0; index < adapters.size(); ++index) {
            const auto& adapter = adapters[index];
            const bool software =
                (adapter.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            std::cout << "adapter_" << index << "_name="
                      << narrow(adapter.description.Description) << '\n';
            std::cout << "adapter_" << index << "_luid="
                      << luid_string(adapter.description.AdapterLuid) << '\n';
            std::cout << "adapter_" << index << "_software="
                      << (software ? "true" : "false") << '\n';
            std::cout << "adapter_" << index << "_dedicated_video_memory="
                      << adapter.description.DedicatedVideoMemory << '\n';
        }

        AdapterRecord* producer = choose_producer(adapters);
        if (producer == nullptr) {
            std::cout << "g1_producer_adapter_visible=false\n";
            std::cout << "probe_complete=true\n";
            return 0;
        }
        std::cout << "g1_producer_adapter_visible=true\n";
        std::cout << "producer_adapter_name=" << narrow(producer->description.Description)
                  << '\n';
        std::cout << "producer_adapter_luid="
                  << luid_string(producer->description.AdapterLuid) << '\n';

        ComPtr<IDXGIAdapter1> consumer_adapter;
        const HRESULT consumer_result = factory->EnumAdapterByLuid(
            consumer_luid, IID_PPV_ARGS(&consumer_adapter));
        if (FAILED(consumer_result)) {
            std::cout << "g1_consumer_adapter_visible=false\n";
            std::cout << "g1_consumer_adapter_hresult="
                      << hresult_string(consumer_result) << '\n';
            std::cout << "g2_r8_texture_status=not-run\n";
            std::cout << "g2_nv12_texture_status=not-run\n";
            std::cout << "g3_buffer_status=not-run\n";
            std::cout << "probe_complete=true\n";
            return 0;
        }

        DXGI_ADAPTER_DESC1 consumer_description{};
        check_hresult(consumer_adapter->GetDesc1(&consumer_description),
                      "consumer GetDesc1");
        std::cout << "g1_consumer_adapter_visible=true\n";
        std::cout << "consumer_adapter_name=" << narrow(consumer_description.Description)
                  << '\n';
        std::cout << "consumer_adapter_luid="
                  << luid_string(consumer_description.AdapterLuid) << '\n';
        std::cout << "consumer_adapter_software="
                  << ((consumer_description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0
                          ? "true"
                          : "false")
                  << '\n';
        std::cout << "cross_adapter_distinct="
                  << (luid_equal(producer->description.AdapterLuid,
                                 consumer_description.AdapterLuid)
                          ? "false"
                          : "true")
                  << '\n';

        ComPtr<ID3D12Device> producer_device;
        ComPtr<ID3D12Device> consumer_device;
        check_hresult(D3D12CreateDevice(producer->adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&producer_device)),
                      "D3D12CreateDevice producer");
        check_hresult(D3D12CreateDevice(consumer_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&consumer_device)),
                      "D3D12CreateDevice consumer");
        print_cross_node_support("producer", producer_device.Get());
        print_cross_node_support("consumer", consumer_device.Get());

        run_case("g3_buffer", [&] {
            test_buffer_copy(producer_device.Get(), consumer_device.Get());
        });
        run_case("g2_r8_texture", [&] {
            test_r8_texture_copy(producer_device.Get(), consumer_device.Get());
        });
        run_case("g2_nv12_texture", [&] {
            test_nv12_texture_open(producer_device.Get(), consumer_device.Get());
        });

        std::cout << "probe_complete=true\n";
        return 0;
    } catch (const HResultError& error) {
        std::cerr << "fatal_hresult=" << hresult_string(error.result()) << '\n';
        std::cerr << "fatal_operation=" << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "fatal_error=" << error.what() << '\n';
        return 1;
    }
}
