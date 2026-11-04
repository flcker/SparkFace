include(FetchContent)

set(ORT_DML_VERSION "1.24.4")
file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" VCPKG_MANIFEST_JSON)
string(JSON VCPKG_BASELINE GET "${VCPKG_MANIFEST_JSON}" builtin-baseline)
set(WINDOWS_SDK_VERSION "$ENV{WindowsSDKVersion}")
string(REGEX REPLACE "\\\\$" "" WINDOWS_SDK_VERSION "${WINDOWS_SDK_VERSION}")
set(ORT_DML_PACKAGE_URL
    "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${ORT_DML_VERSION}/microsoft.ml.onnxruntime.directml.${ORT_DML_VERSION}.nupkg")

FetchContent_Declare(
  onnxruntime_directml
  URL "${ORT_DML_PACKAGE_URL}"
  URL_HASH "SHA256=57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-cmake-project)
FetchContent_MakeAvailable(onnxruntime_directml)

set(ORT_DML_INCLUDE_DIR "${onnxruntime_directml_SOURCE_DIR}/build/native/include")
set(ORT_DML_RUNTIME_DIR "${onnxruntime_directml_SOURCE_DIR}/runtimes/win-x64/native")

add_library(onnxruntime_directml SHARED IMPORTED GLOBAL)
set_target_properties(
  onnxruntime_directml
  PROPERTIES IMPORTED_IMPLIB "${ORT_DML_RUNTIME_DIR}/onnxruntime.lib"
             IMPORTED_LOCATION "${ORT_DML_RUNTIME_DIR}/onnxruntime.dll"
             INTERFACE_INCLUDE_DIRECTORIES "${ORT_DML_INCLUDE_DIR}")

find_package(bgfx CONFIG REQUIRED)

if(TARGET bgfx::bgfx)
  set(SPARKFACE_BGFX_TARGET bgfx::bgfx)
elseif(TARGET bgfx)
  set(SPARKFACE_BGFX_TARGET bgfx)
else()
  message(FATAL_ERROR "The bgfx package did not export a supported CMake target.")
endif()

set(ENV_GATE_MODEL "${CMAKE_CURRENT_BINARY_DIR}/mul_1.onnx")
file(
  DOWNLOAD
  "https://raw.githubusercontent.com/microsoft/onnxruntime/v${ORT_DML_VERSION}/onnxruntime/test/testdata/mul_1.onnx"
  "${ENV_GATE_MODEL}"
  EXPECTED_HASH "SHA256=71f431c4e9321ec6fbeb158d02ed240459a7dcc98673fa79a4f439ce42efaf10"
  TLS_VERIFY ON)

add_executable(env_gate main.cpp)
target_compile_definitions(
  env_gate
  PRIVATE NOMINMAX
          WIN32_LEAN_AND_MEAN
          SF_CMAKE_VERSION="${CMAKE_VERSION}"
          SF_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
          SF_ORT_DML_VERSION="${ORT_DML_VERSION}"
          SF_VCPKG_BASELINE="${VCPKG_BASELINE}"
          SF_WINDOWS_SDK_VERSION="${WINDOWS_SDK_VERSION}")
target_compile_options(env_gate PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(
  env_gate
  PRIVATE ${SPARKFACE_BGFX_TARGET}
          onnxruntime_directml
          d3d12
          dxgi
          mfplat
          mfsensorgroup
          ole32)

add_custom_command(
  TARGET env_gate
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${ORT_DML_RUNTIME_DIR}/onnxruntime.dll" "$<TARGET_FILE_DIR:env_gate>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${ORT_DML_RUNTIME_DIR}/onnxruntime_providers_shared.dll"
          "$<TARGET_FILE_DIR:env_gate>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ENV_GATE_MODEL}"
          "$<TARGET_FILE_DIR:env_gate>/mul_1.onnx"
  VERBATIM)
