add_executable(transport_cross_adapter_probe capabilities/cross_adapter.cpp)
target_compile_definitions(transport_cross_adapter_probe
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_cross_adapter_probe
                       PRIVATE /W4 /WX /permissive- /EHsc)

set(TRANSPORT_D3D11_EVIDENCE_DIR "${PROJECT_SOURCE_DIR}/spikes/d3d11-handoff")

add_executable(transport_source_adapter_producer gpu-handoff/producer.cpp)
target_include_directories(transport_source_adapter_producer
                           PRIVATE "${TRANSPORT_D3D11_EVIDENCE_DIR}")
target_compile_definitions(transport_source_adapter_producer
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_source_adapter_producer
                       PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(transport_source_adapter_producer
                      PRIVATE d3d11 d3dcompiler dxgi)

add_executable(transport_source_adapter_consumer
               "${TRANSPORT_D3D11_EVIDENCE_DIR}/consumer/main.cpp")
target_include_directories(transport_source_adapter_consumer
                           PRIVATE "${TRANSPORT_D3D11_EVIDENCE_DIR}")
target_compile_definitions(transport_source_adapter_consumer
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_source_adapter_consumer
                       PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(transport_source_adapter_consumer
                      PRIVATE mf mfplat mfreadwrite mfuuid ole32)

add_executable(transport_source_adapter_registrar
               "${TRANSPORT_D3D11_EVIDENCE_DIR}/registrar/main.cpp")
target_compile_definitions(transport_source_adapter_registrar
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_source_adapter_registrar
                       PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(transport_source_adapter_registrar
                      PRIVATE advapi32 mf mfplat mfsensorgroup mfuuid ole32)

add_executable(transport_shared_buffer_producer mf-shared-buffer/producer.cpp)
target_include_directories(transport_shared_buffer_producer
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mf-shared-buffer")
target_compile_definitions(transport_shared_buffer_producer
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_shared_buffer_producer
                       PRIVATE /W4 /WX /wd4324 /permissive- /EHsc)
target_link_libraries(transport_shared_buffer_producer PRIVATE advapi32)
target_link_libraries(transport_cross_adapter_probe PRIVATE d3d12 dxgi)

add_executable(transport_activation_local_server
               activation-topology/local_server.cpp)
target_compile_definitions(transport_activation_local_server
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_activation_local_server
                       PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(transport_activation_local_server PRIVATE ole32)

add_executable(transport_codec_fixture codec-bridge/fixture.cpp)
target_compile_definitions(transport_codec_fixture
                           PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(transport_codec_fixture
                       PRIVATE /W4 /WX /permissive- /EHsc)
