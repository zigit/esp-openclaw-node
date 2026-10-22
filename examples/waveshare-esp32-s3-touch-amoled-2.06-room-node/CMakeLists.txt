cmake_minimum_required(VERSION 3.16)

# Credentials overlay (git-ignored, 0600): wired LAST so it overrides
# sdkconfig.defaults. Missing file is fine (fresh checkout uses the
# placeholder pins). Real template: sdkconfig.credentials.example.
set(SKD_CRED "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
if(EXISTS ${SKD_CRED})
  set(ENV{SDKCONFIG_DEFAULTS} "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${SKD_CRED}")
endif()

# MicroLink is consumed as a git submodule (third_party/microlink) tracking
# studioalight/microlink @ openclaw/room-node -- a branch of the upstream
# CamM2325/microlink tree carrying our field-proven fixes as separate
# commits. Its own components/ dir holds the `microlink` component plus the
# `wireguard_lwip` symlink, so pointing EXTRA_COMPONENT_DIRS there gives us
# both, exactly as upstream's own examples do.
set(EXTRA_COMPONENT_DIRS
    "../../components/esp-openclaw-node"
    "../../components/esp-openclaw-node-provisioning"
    "../../components/esp-openclaw-talk"
    "../../components/esp-openclaw-room-node"
    "../../third_party/microlink/components")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openclaw_waveshare_room_node)
