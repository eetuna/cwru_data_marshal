# Mock Clients

Python mock clients for testing the MRI Data Marshal without hardware.

## Available Clients

### pose_client.py

Sends simulated robot pose updates via `POST /pose` (JSON).

```bash
python3 pose_client.py --http http://localhost:8080 --count 50
```

### http_tracker.py

Polls `GET /image/latest` and `GET /pose` to display live data.

```bash
python3 http_tracker.py --http http://localhost:8080
```

## Deleted Clients

The following clients from v1 have been removed:

- **scanner_kspace_client.py** -- folded into the C++ `kspace_streamer`
- **surface_tracker.py** -- robot-domain feature, deferred
- **planner.py** -- robot-domain feature, deferred

## Usage with Docker

From the umbrella repository:

```bash
docker compose -f docker-compose.demo.yml up
```

Or run individual mock clients against a running marshal:

```bash
python3 clients/mocks/pose_client.py --http http://localhost:8080 --count 20
python3 clients/mocks/http_tracker.py --http http://localhost:8080
```
