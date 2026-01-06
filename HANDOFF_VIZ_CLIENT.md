# Handoff: viz_client GUI Improvement

## Task
Modify the viz_client OpenCV GUI to separate the slice number display from the FPS display. Currently they may overlap or coincide when the image is small.

## Current State
The viz_client displays MRI slices with an OpenCV window. The overlay text (slice number and FPS) needs better positioning.

## Requested Change
- **Slice number**: Display at the **top** of the window
- **FPS**: Display at the **bottom** of the window

This ensures the text doesn't overlap when viewing small images (e.g., 128x128).

## File to Modify
`/workspaces/cwru_data_marshal/clients/viz_client/viz_client_main.cpp`

## Current Demo Configuration
The demo script (`scripts/run_demo_simultaneous.sh`) has configurable image size:
```bash
IMAGE_SIZE=128
IMAGE_NSLICES=5
```

## Testing
After making changes, rebuild and test:
```bash
ninja -C build viz_client
./scripts/run_demo_simultaneous.sh
```

## Context
- The viz_client uses OpenCV's `cv::putText()` for overlay
- Small images (128x128) make text overlap more noticeable
- The GUI should remain readable at various image sizes
