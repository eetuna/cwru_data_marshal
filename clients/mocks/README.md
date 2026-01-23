# Mock Clients

Mock clients for testing the MRI Data Marshal system without hardware.

## Pure Python Clients (No Dependencies)

### ecg_client.py
Simulates ECG signals. POST to `/v1/bio/signal`.

```bash
# Send 10 signals
python3 ecg_client.py --count 10

# Custom endpoint and heart rate
python3 ecg_client.py --endpoint http://localhost:8080 --heart-rate 80 --count 20
```

### pose_client.py
Simulates robot/tracker poses. POST to `/v1/pose/update`.

```bash
# Send 50 pose updates
python3 pose_client.py --count 50

# Circular trajectory at 10Hz
python3 pose_client.py --trajectory circular --interval 0.1 --count 100
```

## Clients Requiring Dependencies

### http_tracker.py
Polls MRI/pose data via HTTP. Requires: `pip install requests`

### planner.py
WebSocket frame continuity checker. Requires: `pip install websockets`

### surface_tracker.py
Mock surface tracker. Requires: `pip install websockets`

## Usage with Docker

```bash
# Start Docker containers
cd /workspaces/cwru_data_marshal
docker compose up -d

# Run mock clients
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/ecg_client.py --count 5
python3 /workspaces/mri_data_marshal_worktree/clients/mocks/pose_client.py --count 20
```
