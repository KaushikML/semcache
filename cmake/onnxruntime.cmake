# Fetches the official prebuilt ONNX Runtime release (pinned version + SHA-256, ADR-003) and
# exposes it as the imported target onnxruntime::onnxruntime.

set(SEMCACHE_ORT_VERSION "1.20.1")
set(SEMCACHE_ORT_SHA256 "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105")
set(SEMCACHE_ORT_CACHE_DIR "${CMAKE_SOURCE_DIR}/.cache/onnxruntime" CACHE PATH
    "Where the ONNX Runtime tarball is downloaded and extracted")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  message(FATAL_ERROR "Prebuilt ONNX Runtime is only pinned for linux-x64 (see ADR-003)")
endif()

set(_ort_name "onnxruntime-linux-x64-${SEMCACHE_ORT_VERSION}")
set(_ort_archive "${SEMCACHE_ORT_CACHE_DIR}/${_ort_name}.tgz")
set(_ort_root "${SEMCACHE_ORT_CACHE_DIR}/${_ort_name}")

if(NOT EXISTS "${_ort_root}/include/onnxruntime_cxx_api.h")
  message(STATUS "Downloading ONNX Runtime ${SEMCACHE_ORT_VERSION}")
  file(DOWNLOAD
    "https://github.com/microsoft/onnxruntime/releases/download/v${SEMCACHE_ORT_VERSION}/${_ort_name}.tgz"
    "${_ort_archive}"
    EXPECTED_HASH SHA256=${SEMCACHE_ORT_SHA256}
    TLS_VERIFY ON
    STATUS _ort_status)
  list(GET _ort_status 0 _ort_code)
  if(NOT _ort_code EQUAL 0)
    message(FATAL_ERROR "ONNX Runtime download failed: ${_ort_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_ort_archive}" DESTINATION "${SEMCACHE_ORT_CACHE_DIR}")
endif()

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
  IMPORTED_LOCATION "${_ort_root}/lib/libonnxruntime.so.${SEMCACHE_ORT_VERSION}"
  IMPORTED_SONAME "libonnxruntime.so.1"
  # SYSTEM: ORT headers are not -Werror clean under our warning set.
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ort_root}/include"
  INTERFACE_INCLUDE_DIRECTORIES "${_ort_root}/include")
