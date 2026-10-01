# A current gamescope for a headless probe on a desktop whose distribution has none
# (Ubuntu 24.04) or one too old for --backend headless (Ubuntu 25.04's 3.16.1).
#   docker build -t mw-c5-gamescope:3 scripts/bench/gamescope
FROM archlinux:latest
RUN pacman -Syu --noconfirm --needed gamescope vulkan-radeon mesa vulkan-tools xdotool \
        gst-plugins-base gst-plugins-good libei pipewire xorg-xwayland xorg-xauth \
    && pacman -Scc --noconfirm \
    && useradd -u 1000 -m -G video,render mw
# uid 1000 must have a name: Xwayland lets the local user in by name, and turns an
# anonymous uid's clients away ("Authorization required").
