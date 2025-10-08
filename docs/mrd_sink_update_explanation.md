# MRD Sink Shape-Aware Update Overview

This note documents the behavioral changes introduced when the MRD sink became shape-aware and began emitting dimension-specific files. It also captures the associated updates to the FK client and test coverage so reviewers can understand the intent without reading through the implementation.

## Dimension Normalization

* The sink now normalizes incoming dimensions to guarantee all spatial axes and the channel count are non-zero before proceeding.
* Zero-valued `nz` and channel counts are automatically promoted to `1` so downstream HDF5 allocation logic never encounters invalid extents.
* Dimension normalization is applied both when constructing `MrdFile` instances and when computing chunk sizes to keep behavior consistent.

## Adaptive Chunk Sizing

* Chunk sizes are computed dynamically based on the current geometry and element type.
* The algorithm starts with a chunk that spans a single frame (`{1, channels, nz, ny, nx}`) and iteratively halves the in-plane and slice dimensions until the total chunk payload fits within an 8&nbsp;MiB target budget.
* This ensures MRD datasets remain performant across a wide range of image dimensions without requiring manual tuning.

## Shape-Aware File Rollover

* The sink tracks the canonical geometry of the currently-open MRD file.
* When incoming images arrive with dimensions that differ from the active file, the sink flushes the existing file, increments a generation counter, and starts a new file whose name encodes the new `{nx}x{ny}x{nz}` triplet.
* File names follow the pattern `<canonical>-<nx>x<ny>x<nz>-g####.mrd`, enabling quick identification of the geometry stored within each file.
* If the sink is configured not to roll automatically, it returns an HTTP&nbsp;400 response with a JSON payload describing the dimension mismatch so clients can react accordingly.

## MRD File Initialization Updates

* `MrdFile` now validates that `nx`, `ny`, and `channels` are non-zero at construction time and computes the frame size using the normalized geometry.
* The file writer eagerly creates the `/images` group and `/header` dataset, writing the provided XML header so consumers can interpret the stream without additional metadata.

## Test Coverage

* New unit tests exercise two main scenarios:
  * Rolling over to a new file when the geometry changes mid-stream.
  * Verifying that computed chunk shapes respect the 8&nbsp;MiB target for multiple element types and aspect ratios.
* The tests assert both the naming convention (including geometry stamps and generation indexes) and the chunk dimensions, ensuring regressions are caught quickly.

## FK Client Sinusoidal Trajectory

* The FK client was updated to emit a smooth sinusoidal trajectory for both translation and yaw.
* Position now oscillates along the X and Y axes with a quarter-wavelength phase offset, while yaw follows a single-axis sinusoid.
* The magnitude and frequency were chosen to preserve the original demo cadence while removing abrupt direction changes.

## Operational Impact

* Operators can now stream MRD data of varying sizes without manual reconfiguration—the sink will transparently create new files per geometry.
* Chunking automatically adapts to the incoming data, keeping I/O performant when dimensions increase substantially.
* Downstream consumers gain clearer insight into file contents thanks to geometry-stamped filenames and preserved header metadata.
