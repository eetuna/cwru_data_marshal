#!/bin/bash
# Shared helpers for external robot marshal integration.

set -e

ROBOT_MARSHAL_DIR="${ROBOT_MARSHAL_DIR:-}"
ROBOT_MARSHAL_BRANCH="${ROBOT_MARSHAL_BRANCH:-robot_data_marshal_with_catheter_system_components}"
ROBOT_MARSHAL_WORKTREE="${ROBOT_MARSHAL_WORKTREE:-../robot_data_marshal_catheter_worktree}"
ROBOT_MARSHAL_HOST="${ROBOT_MARSHAL_HOST:-0.0.0.0}"
ROBOT_MARSHAL_PORT="${ROBOT_MARSHAL_PORT:-8081}"

if [ -z "$ROBOT_MARSHAL_DIR" ]; then
    if [ -d "../robot_data_marshal_with_catheter_system_components" ]; then
        ROBOT_MARSHAL_DIR="../robot_data_marshal_with_catheter_system_components"
    else
        ROBOT_MARSHAL_DIR="$ROBOT_MARSHAL_WORKTREE"
    fi
fi

ROBOT_MARSHAL_BIN="${ROBOT_MARSHAL_BIN:-$ROBOT_MARSHAL_DIR/build/robot_marshal_demo}"
ROBOT_CLIENT_A="${ROBOT_CLIENT_A:-$ROBOT_MARSHAL_DIR/build/client-a}"
ROBOT_CLIENT_B="${ROBOT_CLIENT_B:-$ROBOT_MARSHAL_DIR/build/client-b}"
ROBOT_CLIENT_C="${ROBOT_CLIENT_C:-$ROBOT_MARSHAL_DIR/build/client-c}"
ROBOT_CLIENTS="${ROBOT_CLIENTS:-}"

robot_client_names() {
    if [ -f "$ROBOT_MARSHAL_DIR/client-a.cpp" ]; then
        echo "client-a client-b client-c"
        return 0
    fi

    if [ -f "$ROBOT_MARSHAL_DIR/client-catheter-tracking.cpp" ]; then
        echo "client-catheter-tracking client-front-end client-surface-tracking client-planning client-controller"
        return 0
    fi

    return 1
}

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
    local files_dir="."
    if [ -d "$ROBOT_MARSHAL_DIR/files" ]; then
        files_dir="./files"
    fi

    if [ -f "$ROBOT_MARSHAL_DIR/CMakeLists.txt" ]; then
        cmake -S "$ROBOT_MARSHAL_DIR" -B "$ROBOT_MARSHAL_DIR/build" >/dev/null
        cmake --build "$ROBOT_MARSHAL_DIR/build" >/dev/null
    elif [ -f "$ROBOT_MARSHAL_DIR/server.cpp" ]; then
        mkdir -p "$ROBOT_MARSHAL_DIR/build"
        local patched="$ROBOT_MARSHAL_DIR/build/robot_marshal_demo_patched.cpp"
        sed -E \
            -e "s/server.listen\\(\"[^\"]+\", [0-9]+\\)/server.listen(\\\"$ROBOT_MARSHAL_HOST\\\", $ROBOT_MARSHAL_PORT)/" \
            -e 's|"/files/"|"./files/"|g' \
            "$ROBOT_MARSHAL_DIR/server.cpp" > "$patched"
        g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$patched" \
            -o "$ROBOT_MARSHAL_DIR/build/robot_marshal_demo" -lpthread
    else
        return 1
    fi

    printf '%s\n' "port=$ROBOT_MARSHAL_PORT" "files_dir=$files_dir" > "$stamp"
    return 0
}

ensure_robot_marshal_checkout() {
    if [ -d "$ROBOT_MARSHAL_DIR" ]; then
        return 0
    fi

    if [ -n "$ROBOT_MARSHAL_BRANCH" ]; then
        if git rev-parse --verify "$ROBOT_MARSHAL_BRANCH" >/dev/null 2>&1; then
            git worktree add "$ROBOT_MARSHAL_DIR" "$ROBOT_MARSHAL_BRANCH" >/dev/null
            return 0
        fi
    fi

    return 1
}

ensure_robot_marshal_ready() {
    if ensure_robot_marshal_bins "$ROBOT_MARSHAL_BIN"; then
        local stamp="$ROBOT_MARSHAL_DIR/build/.robot_marshal_port"
        if [ -f "$stamp" ]; then
            local built_port
            local built_files_dir
            built_port=$(grep -E "^port=" "$stamp" 2>/dev/null | cut -d= -f2 || true)
            built_files_dir=$(grep -E "^files_dir=" "$stamp" 2>/dev/null | cut -d= -f2 || true)
            local want_files_dir="."
            if [ -d "$ROBOT_MARSHAL_DIR/files" ]; then
                want_files_dir="./files"
            fi
            if [ "$built_port" = "$ROBOT_MARSHAL_PORT" ] && [ "$built_files_dir" = "$want_files_dir" ]; then
                if [ -f "$ROBOT_MARSHAL_DIR/server.cpp" ] && [ "$ROBOT_MARSHAL_DIR/server.cpp" -nt "$ROBOT_MARSHAL_BIN" ]; then
                    : # source updated; rebuild below
                else
                    return 0
                fi
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

    local clients
    if ! clients=$(robot_client_names); then
        return 1
    fi

    mkdir -p "$ROBOT_MARSHAL_DIR/build"
    local stamp="$ROBOT_MARSHAL_DIR/build/.robot_client_port"
    local client_repl="httplib::Client cli(\\\"127.0.0.1\\\", $ROBOT_MARSHAL_PORT);"

    for name in $clients; do
        local src="$ROBOT_MARSHAL_DIR/${name}.cpp"
        local patched="$ROBOT_MARSHAL_DIR/build/${name}_patched.cpp"
        local bin="$ROBOT_MARSHAL_DIR/build/${name}"
        if [ ! -f "$src" ]; then
            return 1
        fi
        sed -E "s/httplib::Client cli\\(\"[^\"]+\", [0-9]+\\);/$client_repl/" "$src" > "$patched"
        g++ -std=c++17 -I "$ROBOT_MARSHAL_DIR" "$patched" -o "$bin" -lpthread
    done

    echo "$ROBOT_MARSHAL_PORT" > "$stamp"
    return 0
}

ensure_robot_clients_ready() {
    local clients
    if ! clients=$(robot_client_names); then
        return 1
    fi

    local bins=()
    for name in $clients; do
        bins+=("$ROBOT_MARSHAL_DIR/build/$name")
    done

    if ensure_robot_marshal_bins "${bins[@]}"; then
        local stamp="$ROBOT_MARSHAL_DIR/build/.robot_client_port"
        if [ -f "$stamp" ]; then
            local built_port
            built_port=$(cat "$stamp" 2>/dev/null || true)
            if [ "$built_port" = "$ROBOT_MARSHAL_PORT" ]; then
                ROBOT_CLIENTS=""
                for name in $clients; do
                    ROBOT_CLIENTS+="${name}:$ROBOT_MARSHAL_DIR/build/$name"$'\n'
                done
                export ROBOT_CLIENTS
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

    ROBOT_CLIENTS=""
    for name in $clients; do
        ROBOT_CLIENTS+="${name}:$ROBOT_MARSHAL_DIR/build/$name"$'\n'
    done
    export ROBOT_CLIENTS
    ensure_robot_marshal_bins "${bins[@]}"
}
