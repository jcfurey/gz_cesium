# Keep the pinned KTX/Basis encoder's SSE translation units free of implicit
# AVX defaults (some Ubuntu 26.04 compiler builds default to x86-64-v3).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_C_FLAGS "-march=x86-64")
set(VCPKG_CXX_FLAGS "-march=x86-64")
