# ============================================================================
# mw-d3d12-lab — measure before building the D3D12 video pipeline.
#
# The second attempt at a D3D12 pipeline (plan pipeline-video-d3d12-v2) starts
# by asking the hardware, not by writing the engine: which rate controls each
# D3D12 Video Encode driver takes, whether the shared fences of the Desktop
# Duplication handshake open both ways and what a round costs, which queue
# priorities are granted and to whom, and which queues can be timestamped.
# Later subcommands time the real conversion on each queue type under a game,
# look for torn frames in the DDA handshake, and encode.
#
# Never installed nor shipped, outside the library's licence boundary: a lab
# instrument, built on demand with -DMW_BUILD_TOOLS=ON. It links
# mw-native-host since the `queues` probe: the probes time the engine's own
# conversion and handshake (ColorConvert, ColorConvert12, DdaInterop) rather
# than a lab copy of them. Nothing of the lab links into the library.
# ============================================================================

add_executable(mw-d3d12-lab
    src/main.cpp
    src/Lab.cpp
    src/Caps.cpp
    src/ComputeConvert.cpp
    src/Queues.cpp
    src/Interop.cpp
)

set_target_properties(mw-d3d12-lab PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(mw-d3d12-lab PRIVATE cxx_std_17)
target_compile_definitions(mw-d3d12-lab PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
# The engine's own sources directory too: the probes use its internal classes.
target_include_directories(mw-d3d12-lab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(mw-d3d12-lab PRIVATE mw-native-host d3d12 d3d11 dxgi d3dcompiler ole32
                                           version advapi32)

if(MSVC)
    target_compile_options(mw-d3d12-lab PRIVATE /W4 /utf-8)
endif()
