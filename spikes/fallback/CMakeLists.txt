find_package(bgfx CONFIG REQUIRED)

if(TARGET bgfx::bgfx)
  set(SPARKFACE_BGFX_TARGET bgfx::bgfx)
elseif(TARGET bgfx)
  set(SPARKFACE_BGFX_TARGET bgfx)
else()
  message(FATAL_ERROR "The bgfx package did not export a supported CMake target.")
endif()

get_target_property(BGFX_INCLUDE_DIRS ${SPARKFACE_BGFX_TARGET}
                    INTERFACE_INCLUDE_DIRECTORIES)
list(GET BGFX_INCLUDE_DIRS 0 BGFX_INCLUDE_DIR)

set(BGFX_SHADERC_CANDIDATES)
foreach(BGFX_SHADERC_ROOT "$ENV{VCPKG_ROOT}" "${PROJECT_SOURCE_DIR}/build/tools/vcpkg")
  file(GLOB_RECURSE BGFX_SHADERC_UNDER_ROOT
       "${BGFX_SHADERC_ROOT}/buildtrees/bgfx/*/cmake/bgfx/shaderc.exe")
  list(APPEND BGFX_SHADERC_CANDIDATES ${BGFX_SHADERC_UNDER_ROOT})
endforeach()
list(REMOVE_DUPLICATES BGFX_SHADERC_CANDIDATES)
list(LENGTH BGFX_SHADERC_CANDIDATES BGFX_SHADERC_COUNT)
if(BGFX_SHADERC_COUNT EQUAL 0)
  message(FATAL_ERROR "bgfx shaderc.exe was not found in the pinned vcpkg buildtrees")
endif()
list(GET BGFX_SHADERC_CANDIDATES 0 BGFX_SHADERC)

set(FALLBACK_SHADER "${CMAKE_CURRENT_BINARY_DIR}/cs_fallback.bin")
add_custom_command(
  OUTPUT "${FALLBACK_SHADER}"
  COMMAND
    "${BGFX_SHADERC}" -f "${CMAKE_CURRENT_SOURCE_DIR}/cs_fallback.sc" -o
    "${FALLBACK_SHADER}" --type compute --platform windows -p s_5_0 -i
    "${BGFX_INCLUDE_DIR}/bgfx" --Werror
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cs_fallback.sc"
  VERBATIM)
add_custom_target(fallback_shader DEPENDS "${FALLBACK_SHADER}")

add_executable(fallback_producer producer/main.cpp)
add_dependencies(fallback_producer fallback_shader)
target_include_directories(fallback_producer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fallback_producer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fallback_producer PRIVATE /W4 /WX /wd4324 /permissive- /EHsc)
target_link_libraries(fallback_producer PRIVATE ${SPARKFACE_BGFX_TARGET} d3d11 dxgi user32)
add_custom_command(
  TARGET fallback_producer
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FALLBACK_SHADER}"
          "$<TARGET_FILE_DIR:fallback_producer>/cs_fallback.bin"
  VERBATIM)

add_executable(fallback_consumer consumer/main.cpp)
target_include_directories(fallback_consumer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fallback_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fallback_consumer PRIVATE /W4 /WX /wd4324 /permissive- /EHsc)

add_executable(fallback_mf_consumer consumer/mf_main.cpp)
target_compile_definitions(fallback_mf_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fallback_mf_consumer PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(fallback_mf_consumer PRIVATE mf mfplat mfreadwrite mfuuid ole32)

add_executable(fallback_registrar registrar/main.cpp)
target_compile_definitions(fallback_registrar PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fallback_registrar PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(fallback_registrar PRIVATE advapi32 mf mfplat mfsensorgroup mfuuid ole32)
