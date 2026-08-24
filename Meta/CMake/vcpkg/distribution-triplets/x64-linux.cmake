include (${CMAKE_CURRENT_LIST_DIR}/../base-triplets/x64-linux.cmake)
include (${CMAKE_CURRENT_LIST_DIR}/distribution.cmake)

# ffmpeg's hand-written assembly is not position-independent, so its static archives cannot be
# linked into a shared library. Ship it as a private shared library instead.
if (PORT STREQUAL "ffmpeg")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
