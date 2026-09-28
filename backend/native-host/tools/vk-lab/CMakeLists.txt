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

# The probes that run shaders take their SPIR-V from glslangValidator at build
# time (the Vulkan SDK's, or the distribution's glslang-tools): a header of
# 32-bit words, nothing compiled at run time. Without it, `queues` is left out.
find_program(MW_GLSLANG_VALIDATOR glslangValidator)
if(MW_GLSLANG_VALIDATOR)
    set(_mw_vk_lab_spv ${CMAKE_CURRENT_BINARY_DIR}/spirv)
    add_custom_command(
        OUTPUT ${_mw_vk_lab_spv}/ConvertSpv.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_mw_vk_lab_spv}
        COMMAND ${MW_GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn kConvertSpv
                -o ${_mw_vk_lab_spv}/ConvertSpv.h ${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert.comp
        VERBATIM)
    target_sources(mw-vk-lab PRIVATE src/Queues.cpp ${_mw_vk_lab_spv}/ConvertSpv.h)
    target_include_directories(mw-vk-lab PRIVATE ${_mw_vk_lab_spv})
    target_compile_definitions(mw-vk-lab PRIVATE MW_VK_LAB_SHADERS=1)
else()
    message(STATUS "mw-vk-lab: no glslangValidator, built without `queues`")
endif()

# `egl`, the witness of `queues`: the chain of today in GLES, as GlConvert sets
# it up. Needs what the engine's Linux graphics backend links (EGL, GLESv2, GBM).
pkg_check_modules(MW_VK_LAB_GL egl glesv2 gbm)
if(MW_VK_LAB_GL_FOUND)
    target_sources(mw-vk-lab PRIVATE src/Egl.cpp)
    target_include_directories(mw-vk-lab SYSTEM PRIVATE ${MW_VK_LAB_GL_INCLUDE_DIRS})
    target_link_libraries(mw-vk-lab PRIVATE ${MW_VK_LAB_GL_LIBRARIES})
    target_compile_definitions(mw-vk-lab PRIVATE MW_VK_LAB_EGL=1)
else()
    message(STATUS "mw-vk-lab: no EGL/GLESv2/GBM, built without `egl`")
endif()
