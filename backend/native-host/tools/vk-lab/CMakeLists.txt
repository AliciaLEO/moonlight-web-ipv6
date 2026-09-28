# ============================================================================
# mw-vk-lab — measure before building the Linux Vulkan chain.
#
# Phase 13 of the second D3D12 pipeline plan gives the Linux host the shape the
# Windows one got: the capture imported, converted and encoded on the GPU,
# with one CPU wait per frame and queues at a priority of their own — in
# Vulkan (Vulkan Video) where the driver allows it, VA-API otherwise. Like
# mw-d3d12-lab before the D3D12 chain, this asks the hardware first: what
# each Vulkan Video encoder takes (codecs, CTB sizes, references, rate control
# modes, intra-refresh, QP maps), whether the KMS plane and the portal's
# buffers import with their modifiers (AMD's displayable DCC is three planes),
# which queue priorities are granted and to whom, and which queues can be
# timestamped.
#
# Never installed nor shipped, outside the library's licence boundary: a lab
# instrument, built on demand with -DMW_BUILD_TOOLS=ON on Linux. The Vulkan
# loader is opened with dlopen, the way the engine will open it; the headers
# are the module's own (third_party/vulkan-headers).
# ============================================================================

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MW_VK_LAB_DRM libdrm)
endif()
if(NOT MW_VK_LAB_DRM_FOUND)
    message(STATUS "mw-vk-lab skipped: needs libdrm (libdrm-dev)")
    return()
endif()

add_executable(mw-vk-lab
    src/main.cpp
    src/Lab.cpp
    src/Vk.cpp
    src/Caps.cpp
    src/Encode.cpp
)

set_target_properties(mw-vk-lab PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(mw-vk-lab PRIVATE cxx_std_17)
# tools/common holds what the labs share (the JSON writer); the engine's
# sources give `encode` its header-only readers (HevcSliceParser), so that the
# lab reads the driver's bytes the way the engine will.
target_include_directories(mw-vk-lab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                             ${CMAKE_CURRENT_SOURCE_DIR}/../common
                                             ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_include_directories(mw-vk-lab SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/vulkan-headers/include
    ${MW_VK_LAB_DRM_INCLUDE_DIRS})
target_link_libraries(mw-vk-lab PRIVATE ${MW_VK_LAB_DRM_LIBRARIES} ${CMAKE_DL_LIBS})
target_compile_options(mw-vk-lab PRIVATE -Wall -Wextra)
