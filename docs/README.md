# MRI Data Marshal Documentation

This directory contains the official documentation for the MRI Data Marshal project.

## Documentation Structure

### Core Documentation
- **[MRI_DATA_MARSHAL_PRESENTATION.md](MRI_DATA_MARSHAL_PRESENTATION.md)** - Professional presentation for faculty and researchers
- **[DEMO_GUIDE.md](DEMO_GUIDE.md)** - How to run the simultaneous operations demo
- **[USAGE_AND_API.md](USAGE_AND_API.md)** - Complete usage instructions and API reference
- **[IMPROVEMENTS_AND_OPTIMIZATION.md](IMPROVEMENTS_AND_OPTIMIZATION.md)** - Performance tuning and optimization guide

## Quick Links

### For New Users
Start here: [DEMO_GUIDE.md](DEMO_GUIDE.md)
```bash
./scripts/run_demo_simultaneous.sh
```

### For Developers
See: [USAGE_AND_API.md](USAGE_AND_API.md)
- Command-line options
- HTTP API endpoints
- Configuration parameters
- Performance tuning

### For Researchers/Faculty
See: [MRI_DATA_MARSHAL_PRESENTATION.md](MRI_DATA_MARSHAL_PRESENTATION.md)
- Architecture overview
- Performance characteristics
- Use cases and limitations
- Comparison with alternatives

### For Performance Optimization
See: [IMPROVEMENTS_AND_OPTIMIZATION.md](IMPROVEMENTS_AND_OPTIMIZATION.md)
- Current limitations analysis
- Improvement strategies with effort/gain estimates
- Performance debugging guide

## Key Facts

- **Performance:** 19 fps @ 50ms intervals, 40 MB/s throughput
- **Real-time:** SWMR HDF5 with HTTP APIs
- **Multi-client:** Simultaneous visualization + data fusion
- **Storage:** HDF5 with SWMR support
- **Visualization:** OpenCV real-time display with slice navigation

## System Components

1. **MRI Marshal** - Data ingestion and HDF5 SWMR storage
2. **Image Streamer** - Synthetic frame generator
3. **Visualizer** - Real-time OpenCV display
4. **Robot Marshal** - State blackboard for sensor fusion

## Archive

Previous documentation and test data have been archived in the `archive/` folder to keep the repo clean:
- `archive/root_docs/` - Previous markdown documentation
- `archive/docs_backup/` - Previous docs folder contents
- `archive/test_data/` - Test data and experiment directories

All previous work is preserved and can be referenced if needed.
