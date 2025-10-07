# Troubleshooting

- Segfault after /health: health handler must not dereference shared state.
- WS connect refused: ensure ports exposed 8080/8090, route `/ws` matches.
- latest.json missing: verify ingest path writes index & latest atomically.
- CMake can't find Boost/HDF5/ISMRMRD: install `libboost-all-dev libhdf5-dev libismrmrd-dev`
  (Ubuntu/Debian) or make sure the SDK paths are exported via `CMAKE_PREFIX_PATH`.
