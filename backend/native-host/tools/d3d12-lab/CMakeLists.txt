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
# Deliberately NOT linked to mw-native-host, never installed nor shipped, and
# outside the library's licence boundary: a lab instrument, built on demand
# with -DMW_BUILD_TOOLS=ON.
# ============================================================================

add_executable(mw-d3d12-lab
    src/main.cpp
    src/Lab.cpp
    src/Caps.cpp
)

set_target_properties(mw-d3d12-lab PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(mw-d3d12-lab PRIVATE cxx_std_17)
target_compile_definitions(mw-d3d12-lab PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_include_directories(mw-d3d12-lab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mw-d3d12-lab PRIVATE d3d12 d3d11 dxgi ole32 version advapi32)

if(MSVC)
    target_compile_options(mw-d3d12-lab PRIVATE /W4 /utf-8)
endif()
