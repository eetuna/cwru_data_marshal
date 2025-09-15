# cwru_data_marshal
# Data Marshal (Boost.Beast)


## Quickstart
```bash
docker compose build --build-arg BUILDKIT_INLINE_CACHE=1
docker compose up -d
# health
curl -s http://localhost:8080/health
# current pose
curl -s http://localhost:8080/v1/pose/current | jq
# latest MRD
curl -s http://localhost:8080/v1/mrd/latest | jq
# entries since timestamp
curl -s "http://localhost:8080/v1/mrd/since?ts=2025-09-10T12:30:00Z&limit=5" | jq

---

## Documentation
See `docs/` for PURPOSE, ARCHITECTURE, API, DEVELOPMENT, and Doxygen config.

## Docker (curl + jq installed)
- Unified Dockerfile: `docker/Dockerfile` (targets: dev, runtime)
- `curl` and `jq` are installed in both dev and runtime stages.
- Faster builds via distro ISMRMRD (flip `BUILD_ISMRMRD_FROM_SOURCE=true` to build latest).

## Usage (compose)
```
docker compose up --build
```


---

## Build speed & Docker cleanup (2025-09-15)
- **Faster devcontainer** and Docker builds: `--no-install-recommends`, shallow clones, and clean APT caches.
- **PugiXML**: installed automatically (`libpugixml-dev`) and custom `cmake/FindPugiXML.cmake` added; top-level `CMakeLists.txt` now prepends `cmake/` to `CMAKE_MODULE_PATH` so `find_package(PugiXML)` just works.
- **Single Dockerfile**: removed legacy `base.Dockerfile`, `clients.Dockerfile`, etc. Only `docker/Dockerfile` remains.
- **Runtime image**: includes runtime lib `libpugixml1v5` plus `curl` & `jq` for diagnostics.

### Forcing source build of ISMRMRD (optional)
```bash
# Devcontainer or compose args
BUILD_ISMRMRD_FROM_SOURCE=true ISMRMRD_TAG=v1.14.0
```
