#!/bin/bash
# Shared helpers for external robot marshal integration.

set -e

ROBOT_MARSHAL_DIR="${ROBOT_MARSHAL_DIR:-}"
ROBOT_MARSHAL_BRANCH="${ROBOT_MARSHAL_BRANCH:-robot-data-marshal}"
ROBOT_MARSHAL_WORKTREE="${ROBOT_MARSHAL_WORKTREE:-../robot_data_marshal_worktree}"
ROBOT_MARSHAL_HOST="${ROBOT_MARSHAL_HOST:-0.0.0.0}"
ROBOT_MARSHAL_PORT="${ROBOT_MARSHAL_PORT:-8081}"

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

    local stamp="$ROBOT_MARSHAL_DIR/build/.robot_marshal_port"

    if [ -f "$ROBOT_MARSHAL_DIR/CMakeLists.txt" ]; then
        cmake -S "$ROBOT_MARSHAL_DIR" -B "$ROBOT_MARSHAL_DIR/build" >/dev/null
        cmake --build "$ROBOT_MARSHAL_DIR/build" >/dev/null
    elif [ -f "$ROBOT_MARSHAL_DIR/server.cpp" ]; then
        mkdir -p "$ROBOT_MARSHAL_DIR/build"
        local patched="$ROBOT_MARSHAL_DIR/build/robot_marshal_demo_patched.cpp"
        sed -E "s/server.listen\\(\"[^\"]+\", [0-9]+\\)/server.listen(\\\"$ROBOT_MARSHAL_HOST\\\", $ROBOT_MARSHAL_PORT)/" \
            "$ROBOT_MARSHAL_DIR/server.cpp" > "$patched"
        g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$patched" \
            -o "$ROBOT_MARSHAL_DIR/build/robot_marshal_demo" -lpthread
    else
        return 1
    fi

    echo "$ROBOT_MARSHAL_PORT" > "$stamp"
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
        local stamp="$ROBOT_MARSHAL_DIR/build/.robot_marshal_port"
        if [ -f "$stamp" ]; then
            local built_port
            built_port=$(cat "$stamp" 2>/dev/null || true)
            if [ "$built_port" = "$ROBOT_MARSHAL_PORT" ]; then
                return 0
            fi
        fi
    fi

    if ! ensure_robot_marshal_checkout; then
        return 1
    fi

    if ! try_build_robot_marshal; then
        return 1
    fi

    ensure_robot_marshal_bins "$ROBOT_MARSHAL_BIN"
}

try_build_robot_clients() {
    if [ ! -d "$ROBOT_MARSHAL_DIR" ]; then
        return 1
    fi

    if [ ! -f "$ROBOT_MARSHAL_DIR/client-a.cpp" ] || [ ! -f "$ROBOT_MARSHAL_DIR/client-b.cpp" ] || [ ! -f "$ROBOT_MARSHAL_DIR/client-c.cpp" ]; then
        return 1
    fi

    mkdir -p "$ROBOT_MARSHAL_DIR/build"
    local stamp="$ROBOT_MARSHAL_DIR/build/.robot_client_port"
    local client_a="$ROBOT_MARSHAL_DIR/build/client-a_patched.cpp"
    local client_b="$ROBOT_MARSHAL_DIR/build/client-b_patched.cpp"
    local client_c="$ROBOT_MARSHAL_DIR/build/client-c_patched.cpp"
    local client_repl="httplib::Client cli(\\\"127.0.0.1\\\", $ROBOT_MARSHAL_PORT);"

    sed -E "s/httplib::Client cli\\(\"[^\"]+\", [0-9]+\\);/$client_repl/" \
        "$ROBOT_MARSHAL_DIR/client-a.cpp" > "$client_a"
    sed -E "s/httplib::Client cli\\(\"[^\"]+\", [0-9]+\\);/$client_repl/" \
        "$ROBOT_MARSHAL_DIR/client-b.cpp" > "$client_b"
    sed -E "s/httplib::Client cli\\(\"[^\"]+\", [0-9]+\\);/$client_repl/" \
        "$ROBOT_MARSHAL_DIR/client-c.cpp" > "$client_c"

    g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$client_a" -o "$ROBOT_MARSHAL_DIR/build/client-a" -lpthread
    g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$client_b" -o "$ROBOT_MARSHAL_DIR/build/client-b" -lpthread
    g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$client_c" -o "$ROBOT_MARSHAL_DIR/build/client-c" -lpthread

    echo "$ROBOT_MARSHAL_PORT" > "$stamp"
    return 0
}

ensure_robot_clients_ready() {
    if ensure_robot_marshal_bins "$ROBOT_CLIENT_A" "$ROBOT_CLIENT_B" "$ROBOT_CLIENT_C"; then
        local stamp="$ROBOT_MARSHAL_DIR/build/.robot_client_port"
        if [ -f "$stamp" ]; then
            local built_port
            built_port=$(cat "$stamp" 2>/dev/null || true)
            if [ "$built_port" = "$ROBOT_MARSHAL_PORT" ]; then
                return 0
            fi
        fi
    fi

    if ! ensure_robot_marshal_checkout; then
        return 1
    fi

    if ! try_build_robot_clients; then
        return 1
    fi

    ensure_robot_marshal_bins "$ROBOT_CLIENT_A" "$ROBOT_CLIENT_B" "$ROBOT_CLIENT_C"
}
