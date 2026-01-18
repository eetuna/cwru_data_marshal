#!/bin/bash
# Shared helpers for external robot marshal integration.

set -e

ROBOT_MARSHAL_DIR="${ROBOT_MARSHAL_DIR:-}"
ROBOT_MARSHAL_BRANCH="${ROBOT_MARSHAL_BRANCH:-robot-data-marshal}"
ROBOT_MARSHAL_WORKTREE="${ROBOT_MARSHAL_WORKTREE:-../robot_data_marshal_worktree}"

if [ -z "$ROBOT_MARSHAL_DIR" ]; then
    if [ -d "../robot_data_marshal_with_catheter_system_components" ]; then
        ROBOT_MARSHAL_DIR="../robot_data_marshal_with_catheter_system_components"
    elif [ -d "../robot_data_marshal" ]; then
        ROBOT_MARSHAL_DIR="../robot_data_marshal"
    else
        ROBOT_MARSHAL_DIR="$ROBOT_MARSHAL_WORKTREE"
    fi
fi

ROBOT_MARSHAL_BIN="${ROBOT_MARSHAL_BIN:-$ROBOT_MARSHAL_DIR/build/robot_marshal_demo}"
ROBOT_CLIENT_A="${ROBOT_CLIENT_A:-$ROBOT_MARSHAL_DIR/build/client-a}"
ROBOT_CLIENT_B="${ROBOT_CLIENT_B:-$ROBOT_MARSHAL_DIR/build/client-b}"
ROBOT_CLIENT_C="${ROBOT_CLIENT_C:-$ROBOT_MARSHAL_DIR/build/client-c}"

ensure_robot_marshal_bins() {
    for bin in "$@"; do
        if [ -z "$bin" ] || [ ! -x "$bin" ]; then
            return 1
        fi
    done
    return 0
}

try_build_robot_marshal() {
    if [ ! -d "$ROBOT_MARSHAL_DIR" ]; then
        return 1
    fi

    if [ -f "$ROBOT_MARSHAL_DIR/CMakeLists.txt" ]; then
        cmake -S "$ROBOT_MARSHAL_DIR" -B "$ROBOT_MARSHAL_DIR/build" >/dev/null
        cmake --build "$ROBOT_MARSHAL_DIR/build" >/dev/null
    elif [ -f "$ROBOT_MARSHAL_DIR/server.cpp" ]; then
        mkdir -p "$ROBOT_MARSHAL_DIR/build"
        g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$ROBOT_MARSHAL_DIR/server.cpp" \
            -o "$ROBOT_MARSHAL_DIR/build/robot_marshal_demo" -lpthread
    else
        return 1
    fi

    return 0
}

ensure_robot_marshal_checkout() {
    if [ -d "$ROBOT_MARSHAL_DIR" ]; then
        return 0
    fi

    if git rev-parse --verify "$ROBOT_MARSHAL_BRANCH" >/dev/null 2>&1; then
        git worktree add "$ROBOT_MARSHAL_DIR" "$ROBOT_MARSHAL_BRANCH" >/dev/null
        return 0
    fi

    return 1
}

ensure_robot_marshal_ready() {
    if ensure_robot_marshal_bins "$ROBOT_MARSHAL_BIN"; then
        return 0
    fi

    if ! ensure_robot_marshal_checkout; then
        return 1
    fi

    if ! try_build_robot_marshal; then
        return 1
    fi

    ensure_robot_marshal_bins "$ROBOT_MARSHAL_BIN"
}
