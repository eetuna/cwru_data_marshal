# Troubleshooting

- Segfault after /health: health handler must not dereference shared state.
- WS connect refused: ensure ports exposed 8080/8090, route `/ws` matches.
- latest.json missing: verify ingest path writes index & latest atomically.
- CMake can't find Boost/HDF5/ISMRMRD: install `libboost-all-dev libhdf5-dev libismrmrd-dev`
  (Ubuntu/Debian) or make sure the SDK paths are exported via `CMAKE_PREFIX_PATH`.
- viz_client disabled because OpenCV is missing: install `libopencv-dev libopencv-viz-dev`
  in addition to the core dependencies above, then rerun `cmake -S . -B build` (and rebuild with
  `cmake --build build --target viz_client`). If OpenCV is installed in a non-standard
  prefix, set `OpenCV_DIR` to the folder containing `OpenCVConfig.cmake`. The `libopencv-dev`
  meta package is what provides `OpenCVConfig.cmake`; the devcontainer/Docker images still add the
  split `libopencv-*-dev` packages so we get the viz GUI bits without pulling in every optional
  OpenCV module via `Recommends`. A healthy configure run prints
  `Found OpenCV: <version> (linking via targets .../libraries ...)` and the deduplicated include
  directories so you can confirm CMake located the viz component.
- `image_streamer` reconnect loop prints `write failed (need buffer)` after the release that
  introduced `--flush-max-frames/--flush-max-ms`: that patch also switched the client to use
  `boost::beast::http::buffer_body` without providing an explicit dynamic serializer buffer. When
  you post a ~0.5&nbsp;MiB frame (128×128×8 float volume + header) the synchronous
  `http::write(stream, req, ec)` overload reports `boost::beast::http::error::need_buffer` because
  the fixed 8&nbsp;KiB internal workspace cannot hold the payload. The marshal options simply
  batch disk flushes; they do not enlarge the HTTP body. Rebuild the client so it calls the
  overload that accepts a `flat_buffer` (or fall back to `http::vector_body`) or temporarily shrink
  the frame dimensions to avoid the serializer limit. See
  `clients/image_streamer/image_streamer_main.cpp` for the call site.
- Worried that clearing the HTTP buffers every frame will slow the streamer down? `flat_buffer::consume`
  only moves the readable window back to zero – it keeps the underlying allocation, so the next
  request reuses the same memory instead of allocating. The client also reserves enough workspace
  up front (`write_buffer.reserve(...)`) for a 128×128×8 float frame, so throughput matches the
  pre-regression build while the explicit buffer still guarantees no `need_buffer` errors regardless
  of the marshal flush settings.
