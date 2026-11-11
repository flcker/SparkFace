include(FetchContent)

set(ORT_DML_VERSION "1.24.4")
set(ORT_DML_PACKAGE_URL
    "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${ORT_DML_VERSION}/microsoft.ml.onnxruntime.directml.${ORT_DML_VERSION}.nupkg")

if(NOT TARGET onnxruntime_directml)
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
endif()

get_target_property(ORT_DML_RUNTIME_LIBRARY onnxruntime_directml IMPORTED_LOCATION)
get_filename_component(ORT_DML_RUNTIME_DIR "${ORT_DML_RUNTIME_LIBRARY}" DIRECTORY)

add_executable(face_spike main.cpp preprocess.cpp landmarks.cpp)
target_compile_definitions(face_spike PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
                                             SF_ORT_DML_VERSION="${ORT_DML_VERSION}")
target_compile_options(face_spike PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(face_spike PRIVATE onnxruntime_directml d3d12 dxgi psapi)

add_custom_command(
  TARGET face_spike
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${ORT_DML_RUNTIME_DIR}/onnxruntime.dll" "$<TARGET_FILE_DIR:face_spike>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${ORT_DML_RUNTIME_DIR}/onnxruntime_providers_shared.dll"
          "$<TARGET_FILE_DIR:face_spike>"
  VERBATIM)
