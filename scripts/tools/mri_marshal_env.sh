#!/bin/bash
# Shared helpers for external MRI marshal integration.

set -e

MRI_MARSHAL_DIR="${MRI_MARSHAL_DIR:-}"
MRI_MARSHAL_BRANCH="${MRI_MARSHAL_BRANCH:-mri-data-marhsal}"
MRI_MARSHAL_WORKTREE="${MRI_MARSHAL_WORKTREE:-../mri_data_marshal_worktree}"

if [ -z "$MRI_MARSHAL_DIR" ]; then
    MRI_MARSHAL_DIR="$MRI_MARSHAL_WORKTREE"
fi

MRI_BUILD_DIR="${MRI_BUILD_DIR:-$MRI_MARSHAL_DIR/build}"
MRI_MARSHAL_BIN="${MRI_MARSHAL_BIN:-$MRI_BUILD_DIR/marshal}"
MRI_IMAGE_STREAMER_BIN="${MRI_IMAGE_STREAMER_BIN:-$MRI_BUILD_DIR/image_streamer}"
MRI_VIZ_CLIENT_BIN="${MRI_VIZ_CLIENT_BIN:-$MRI_BUILD_DIR/viz_client}"
MRI_MK_MRD_BIN="${MRI_MK_MRD_BIN:-$MRI_BUILD_DIR/mk_mrd}"
MRI_PLAYBACK_BIN="${MRI_PLAYBACK_BIN:-$MRI_BUILD_DIR/playback}"

ensure_mri_bins() {
    for bin in "$@"; do
        if [ -z "$bin" ] || [ ! -x "$bin" ]; then
            return 1
        fi
    done
    return 0
}

ensure_mri_checkout() {
    if [ -d "$MRI_MARSHAL_DIR" ]; then
        return 0
    fi

    if git rev-parse --verify "$MRI_MARSHAL_BRANCH" >/dev/null 2>&1; then
        git worktree add "$MRI_MARSHAL_DIR" "$MRI_MARSHAL_BRANCH" >/dev/null
        return 0
    fi

    return 1
}

try_build_mri() {
    if [ ! -d "$MRI_MARSHAL_DIR" ]; then
        return 1
    fi

    if [ ! -f "$MRI_MARSHAL_DIR/CMakeLists.txt" ]; then
        return 1
    fi

    cmake -S "$MRI_MARSHAL_DIR" -B "$MRI_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$MRI_BUILD_DIR" >/dev/null
    return 0
}

ensure_mri_ready() {
    if ensure_mri_bins "$MRI_MARSHAL_BIN" "$MRI_IMAGE_STREAMER_BIN" "$MRI_VIZ_CLIENT_BIN" "$MRI_MK_MRD_BIN" "$MRI_PLAYBACK_BIN"; then
        local needs_rebuild=0
        if [ -f "$MRI_MARSHAL_DIR/src/marshal_main.cpp" ] && [ "$MRI_MARSHAL_DIR/src/marshal_main.cpp" -nt "$MRI_MARSHAL_BIN" ]; then
            needs_rebuild=1
        fi
        if [ -f "$MRI_MARSHAL_DIR/clients/viz_client/viz_client_main.cpp" ] && [ "$MRI_MARSHAL_DIR/clients/viz_client/viz_client_main.cpp" -nt "$MRI_VIZ_CLIENT_BIN" ]; then
            needs_rebuild=1
        fi
        if [ -f "$MRI_MARSHAL_DIR/clients/image_streamer/image_streamer_main.cpp" ] && [ "$MRI_MARSHAL_DIR/clients/image_streamer/image_streamer_main.cpp" -nt "$MRI_IMAGE_STREAMER_BIN" ]; then
            needs_rebuild=1
        fi
        if [ -f "$MRI_MARSHAL_DIR/src/mk_mrd.cpp" ] && [ "$MRI_MARSHAL_DIR/src/mk_mrd.cpp" -nt "$MRI_MK_MRD_BIN" ]; then
            needs_rebuild=1
        fi
        if [ -f "$MRI_MARSHAL_DIR/services/playback/playback_main.cpp" ] && [ "$MRI_MARSHAL_DIR/services/playback/playback_main.cpp" -nt "$MRI_PLAYBACK_BIN" ]; then
            needs_rebuild=1
        fi
        if [ "$needs_rebuild" -eq 0 ]; then
            if [ -f "$MRI_BUILD_DIR/CMakeCache.txt" ]; then
                build_type=$(grep -E '^CMAKE_BUILD_TYPE:STRING=' "$MRI_BUILD_DIR/CMakeCache.txt" | cut -d= -f2- || true)
                if [ "$build_type" = "Release" ]; then
                    return 0
                fi
            fi
        else
            : # rebuild below
        fi
    fi

    if ! ensure_mri_checkout; then
        return 1
    fi

    if ! try_build_mri; then
        return 1
    fi

    ensure_mri_bins "$MRI_MARSHAL_BIN" "$MRI_IMAGE_STREAMER_BIN" "$MRI_VIZ_CLIENT_BIN" "$MRI_MK_MRD_BIN" "$MRI_PLAYBACK_BIN"
}
