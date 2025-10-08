# Container workflows

This guide walks through the container layouts that ship with `cwru_data_marshal` and
how to use them for day-to-day development, ad-hoc testing, and multi-service demos.

## Dockerfile layout (`docker/Dockerfile`)

The repository maintains a single multi-stage Dockerfile with three stages:

| Stage       | Purpose                                                                      |
|-------------|------------------------------------------------------------------------------|
| `build-base`| Full toolchain with compilers, Ninja, Boost, HDF5, ISMRMRD headers, and Python libs for streaming helpers. |
| `dev`       | Extends `build-base` with debugging/diagnostics tools and the `websocat` CLI.|
| `runtime`   | Minimal runtime dependencies for packaging a release image.                  |

Key notes:

- The build stage can compile ISMRMRD from source (`BUILD_ISMRMRD_FROM_SOURCE=true`) or
  fall back to distribution packages. The default `ISMRMRD_TAG=main` matches the marshal's
  SWMR feature set.
- All stages use the same linker configuration (`lld`) and default to the Ninja generator
  so container builds behave the same way as local builds.
- Runtime images copy the `/usr/local` install tree from the build stage so the marshal and
  clients can run without re-compiling inside the container.
- Python 3, `numpy`, `h5py`, and the `ismrmrd` wheel ship in all stages so the lightweight
  streaming helpers (`tools/make_image_message.py`, `tools/stream_image_series.py`) work out
  of the box.

## Dev container (`.devcontainer/devcontainer.json`)

The VS Code devcontainer points to the `dev` stage in the Dockerfile and binds your source
checkout into `/src`. When the container spins up it:

1. Mounts `${workspaceFolder}` into `/src` and `${workspaceFolder}/data` into `/src/data`.
2. Exposes the same Ninja + LLD toolchain that CI uses via `containerEnv`.
3. Creates the standard data directories (`/src/data/mrd`, `/src/data/dumpbox`) via the
   `postCreateCommand` so the marshal can run immediately.

Launch the devcontainer with the "Reopen in Container" command from VS Code. Once the
terminal prompt appears you can build and test as usual:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

## Docker Compose (`docker-compose.yml`)

`docker-compose.yml` defines a four-service demo stack that reuses the `dev` image for each
binary:

- `marshal` — exposes HTTP (8080) and WebSocket (8090) endpoints to the host.
- `viz` — live SWMR reader that trails MRD files produced by the marshal.
- `image_streamer` — continuous multi-slice generator that exercises the HTTP ingest path.
- `fs_tail` — tail-style reader for the MRD index JSON log.

All services share the same bind mounts:

- `.:/src` so containers see the current working copy (sources + build directory).
- `./data:/data` so MRD artifacts persist on the host.

Each container runs the helper command stored in the `BUILD_CMD` environment variable.
By default this command configures and builds the tree with Ninja before launching the
binary. You can override the build step to speed up iteration, for example:

```bash
BUILD_CMD="cmake --build build" docker compose up marshal
```

### Running the full stack

```bash
# Ensure the shared data directory exists on the host
mkdir -p data/mrd data/dumpbox

# Build images and start all services in the foreground
docker compose up --build
```

Once the stack is running:

- `marshal` listens on `http://localhost:8080` and `ws://localhost:8090/ws`.
- `viz` tails `/data/mrd/*.mrd` inside the shared volume and prints frame updates.
- `fk` streams sinusoidal pose updates to `/v1/pose/update` so clients can observe motion.
- `fs_tail` streams updates from `/data/mrd/index.jsonl`.

Use `docker compose logs -f <service>` to inspect each component individually. Shutdown the
stack with `Ctrl+C` or `docker compose down`.

### Building a runtime image

To build a lean runtime container that only ships the compiled binaries, target the
`runtime` stage:

```bash
docker build --target runtime -t cwru-data-marshal:runtime .
```

After building, copy your host-side `build/` artifacts into `/app` or add a small wrapper
script that downloads the desired release bundle.

## Troubleshooting

- **Boost package errors**: the runtime stage pins Boost 1.74 (Ubuntu 22.04). If you change
  `UBUNTU_VERSION`, adjust the Boost package names to match the distro you target.
- **Recompiles on every `docker compose up`**: mount a persistent `build/` directory into
  the containers or override `BUILD_CMD` to only run `cmake --build` once configuration is
  in place.
- **Volume permission issues**: the devcontainer and compose files run as `root` by default.
  If you need non-root users, adjust `remoteUser` in the devcontainer and `user:` fields in
  the compose services consistently.
