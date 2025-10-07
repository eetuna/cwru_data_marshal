# Troubleshooting

- Segfault after /health: health handler must not dereference shared state.
- WS connect refused: ensure ports exposed 8080/8090, route `/ws` matches.
- latest.json missing: verify ingest path writes index & latest atomically.
- CMake can't find Boost/HDF5/ISMRMRD: install `libboost-all-dev libhdf5-dev libismrmrd-dev`
  (Ubuntu/Debian) or make sure the SDK paths are exported via `CMAKE_PREFIX_PATH`.
- viz_client disabled because OpenCV is missing: install `libopencv-dev` in addition to
  the core dependencies above, then rerun `cmake -S . -B build` (and rebuild with
  `cmake --build build --target viz_client`). If OpenCV is installed in a non-standard
  prefix, set `OpenCV_DIR` to the folder containing `OpenCVConfig.cmake`.
