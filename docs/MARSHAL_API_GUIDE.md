# CWRU Data Marshal API Guide

Complete reference for integrating external clients with MRI Marshal and Robot Marshal.

---

## Table of Contents

- [Quick Start](#quick-start)
- [MRI Marshal](#mri-marshal)
  - [HTTP API](#mri-http-api)
  - [WebSocket API](#mri-websocket-api)
  - [Data Formats](#mri-data-formats)
- [Robot Marshal](#robot-marshal)
  - [HTTP API](#robot-http-api)
  - [Data Formats](#robot-data-formats)
- [Client Examples](#client-examples)
- [Troubleshooting](#troubleshooting)

---

## Quick Start

### Connection Endpoints

```
MRI Marshal HTTP:      http://localhost:8080
MRI Marshal WebSocket: ws://localhost:8090
Robot Marshal HTTP:    http://localhost:8081
```

### Test Connection

```bash
# Test MRI Marshal
curl http://localhost:8080/health

# Test Robot Marshal
curl http://localhost:8081/
```

---

## MRI Marshal

The MRI Marshal serves MRI image data, ECG signals, and pose tracking information via HTTP and WebSocket APIs.

### MRI HTTP API

#### Health Check

```http
GET /health
```

**Response:**
```json
{
  "status": "ok",
  "data": {
    "uptime_s": 123.45
  }
}
```

#### Get Latest MRD Data

```http
GET /v1/mrd/latest
```

**Response:** Binary MRD (ISMRMRD) file

**Example:**
```bash
curl -o latest.mrd http://localhost:8080/v1/mrd/latest
```

#### Get MRD Header

```http
GET /v1/mrd/latest/header
```

**Response:**
```json
{
  "version": 1,
  "subjectInformation": {
    "patientName": "Anonymous",
    "patientWeight_kg": 70.0,
    "patientHeight_m": 1.75
  },
  "acquisitionSystemInformation": {
    "systemVendor": "CWRU",
    "systemModel": "Demo",
    "systemFieldStrength_T": 3.0,
    "relativeReceiverNoiseBandwidth": 0.793,
    "receiverChannels": 1
  },
  "experimentalConditions": {
    "H1resonanceFrequency_Hz": 127740000
  },
  "encoding": [{
    "trajectory": "cartesian",
    "encodedSpace": {
      "matrixSize": {
        "x": 64,
        "y": 64,
        "z": 5
      },
      "fieldOfView_mm": {
        "x": 256.0,
        "y": 256.0,
        "z": 20.0
      }
    }
  }]
}
```

#### List Available MRD Files

```http
GET /v1/mrd/list
```

**Response:**
```json
[
  {
    "filename": "demo_stream-64x64x5-g0000.mrd",
    "size_bytes": 245760,
    "last_modified": "2026-01-24T02:00:00Z"
  }
]
```

#### Get Specific MRD File

```http
GET /v1/mrd/{filename}
```

**Example:**
```bash
curl -o data.mrd http://localhost:8080/v1/mrd/demo_stream-64x64x5-g0000.mrd
```

#### Get Latest ECG Data

```http
GET /v1/bio/latest
```

**Response:**
```json
{
  "client_id": "ecg_client",
  "sent_at": 1706140800000,
  "values": [0.5, 0.6, 0.7],
  "heart_rate": 72
}
```

#### Get ECG Stream

```http
GET /v1/bio/stream
```

**Response:** JSONL stream (one JSON object per line)
```jsonl
{"client_id":"ecg_client","sent_at":1706140800000,"values":[0.5],"heart_rate":72}
{"client_id":"ecg_client","sent_at":1706140801000,"values":[0.6],"heart_rate":72}
```

#### Get Latest Pose Data

```http
GET /v1/poses/latest
```

**Response:**
```json
{
  "client_id": "pose_client",
  "sent_at": 1706140800000,
  "position": {
    "x": 10.5,
    "y": 20.3,
    "z": 5.2
  },
  "orientation": {
    "roll": 0.0,
    "pitch": 0.1,
    "yaw": 0.2
  }
}
```

#### Get Pose Stream

```http
GET /v1/poses/stream
```

**Response:** JSONL stream
```jsonl
{"client_id":"pose_client","sent_at":1706140800000,"position":{"x":10.5,"y":20.3,"z":5.2}}
{"client_id":"pose_client","sent_at":1706140801000,"position":{"x":10.6,"y":20.4,"z":5.3}}
```

#### Send ECG Data (Client → Marshal)

```http
POST /v1/bio
Content-Type: application/json
```

**Request Body:**
```json
{
  "client_id": "my_ecg_client",
  "sent_at": 1706140800000,
  "values": [0.5, 0.6, 0.7],
  "heart_rate": 72
}
```

**Response:**
```json
{
  "status": "ok"
}
```

#### Send Pose Data (Client → Marshal)

```http
POST /v1/poses
Content-Type: application/json
```

**Request Body:**
```json
{
  "client_id": "my_pose_client",
  "sent_at": 1706140800000,
  "position": {
    "x": 10.5,
    "y": 20.3,
    "z": 5.2
  },
  "orientation": {
    "roll": 0.0,
    "pitch": 0.1,
    "yaw": 0.2
  }
}
```

### MRI WebSocket API

Connect to: `ws://localhost:8090/ws`

**Real-time MRD updates:**

```javascript
const ws = new WebSocket('ws://localhost:8090/ws');

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  console.log('New MRD data:', data.filename);
  // data contains: { filename, size_bytes, timestamp, ... }
};
```

**Python Example:**
```python
import websocket
import json

def on_message(ws, message):
    data = json.loads(message)
    print(f"New MRD: {data['filename']}")

ws = websocket.WebSocketApp("ws://localhost:8090/ws",
                            on_message=on_message)
ws.run_forever()
```

### MRI Data Formats

#### ECG Data Format

```json
{
  "client_id": "string",        // Client identifier
  "sent_at": 1706140800000,     // Unix timestamp (ms)
  "values": [0.5, 0.6, 0.7],    // ECG voltage values
  "heart_rate": 72              // Optional: beats per minute
}
```

#### Pose Data Format

```json
{
  "client_id": "string",
  "sent_at": 1706140800000,
  "position": {
    "x": 10.5,                  // mm
    "y": 20.3,
    "z": 5.2
  },
  "orientation": {
    "roll": 0.0,                // radians
    "pitch": 0.1,
    "yaw": 0.2
  }
}
```

#### MRD (ISMRMRD) Format

Binary format following ISMRMRD specification v1.13.7.
- Header: XML metadata
- Data: Acquisition records with k-space data
- See: https://ismrmrd.github.io/

---

## Robot Marshal

The Robot Marshal serves robot tracking and control data for catheter navigation systems.

### Robot HTTP API

#### Health Check / Status

```http
GET /
```

**Response:** HTML status page or JSON (depending on Accept header)

**JSON Response:**
```json
{
  "status": "ok",
  "services": [
    "catheter_tracking",
    "controller",
    "planning",
    "front_end",
    "surface_tracking"
  ]
}
```

#### Read Robot Data

```http
GET /read/{filename}
```

**Common files:**
- `/read/robot_status` - Current robot status
- `/read/robot_commands` - Command queue
- `/read/catheter_position` - Catheter tracking data
- `/read/controller_state` - Controller state
- `/read/planning_trajectory` - Planned trajectory
- `/read/surface_data` - Surface tracking data

**Example:**
```bash
curl http://localhost:8081/read/robot_status
```

**Response:**
```json
{
  "client_id": "catheter_tracking",
  "sent_at": 1706140800000,
  "values": [10.5, 20.3, 5.2]
}
```

#### Write Robot Data

```http
POST /write/{filename}
Content-Type: application/json
```

**Request Body:**
```json
{
  "client_id": "my_robot_client",
  "sent_at": 1706140800000,
  "values": [1.0, 2.0, 3.0]
}
```

**Example:**
```bash
curl -X POST http://localhost:8081/write/robot_commands \
  -H "Content-Type: application/json" \
  -d '{"client_id":"my_client","sent_at":1706140800,"values":[1,2,3]}'
```

**Response:**
```
OK
```

#### List Available Files

```http
GET /files
```

**Response:**
```json
[
  "robot_status",
  "robot_commands",
  "catheter_position",
  "controller_state",
  "planning_trajectory",
  "surface_data"
]
```

### Robot Data Formats

#### Robot Status

```json
{
  "client_id": "string",
  "sent_at": 1706140800000,
  "values": [x, y, z]           // Position coordinates (mm)
}
```

#### Command Format

```json
{
  "client_id": "string",
  "sent_at": 1706140800000,
  "values": [cmd1, cmd2, cmd3]  // Command parameters
}
```

---

## Client Examples

### Python: Read MRI Data

```python
import requests
import json

# Get latest MRD header
response = requests.get('http://localhost:8080/v1/mrd/latest/header')
header = response.json()
print(f"Field strength: {header['acquisitionSystemInformation']['systemFieldStrength_T']} T")

# Get latest ECG
ecg = requests.get('http://localhost:8080/v1/bio/latest').json()
print(f"Heart rate: {ecg['heart_rate']} BPM")

# Download MRD file
mrd_data = requests.get('http://localhost:8080/v1/mrd/latest').content
with open('data.mrd', 'wb') as f:
    f.write(mrd_data)
```

### Python: Send ECG Data

```python
import requests
import time

endpoint = 'http://localhost:8080/v1/bio'

def send_ecg(heart_rate, values):
    data = {
        'client_id': 'my_ecg_sensor',
        'sent_at': int(time.time() * 1000),
        'values': values,
        'heart_rate': heart_rate
    }
    response = requests.post(endpoint, json=data)
    return response.json()

# Send ECG reading
result = send_ecg(72, [0.5, 0.6, 0.7])
print(result)  # {'status': 'ok'}
```

### Python: Send Pose Data

```python
import requests
import time

endpoint = 'http://localhost:8080/v1/poses'

def send_pose(x, y, z, roll, pitch, yaw):
    data = {
        'client_id': 'my_tracking_system',
        'sent_at': int(time.time() * 1000),
        'position': {'x': x, 'y': y, 'z': z},
        'orientation': {'roll': roll, 'pitch': pitch, 'yaw': yaw}
    }
    response = requests.post(endpoint, json=data)
    return response.json()

# Send pose update
result = send_pose(10.5, 20.3, 5.2, 0.0, 0.1, 0.2)
print(result)
```

### Python: Robot Marshal Client

```python
import requests
import time

base_url = 'http://localhost:8081'

def read_robot_status():
    response = requests.get(f'{base_url}/read/robot_status')
    return response.json()

def send_robot_command(values):
    data = {
        'client_id': 'my_controller',
        'sent_at': int(time.time() * 1000),
        'values': values
    }
    response = requests.post(f'{base_url}/write/robot_commands', json=data)
    return response.text

# Read status
status = read_robot_status()
print(f"Robot position: {status['values']}")

# Send command
result = send_robot_command([1.0, 2.0, 3.0])
print(result)  # 'OK'
```

### C++: Read MRI Data

```cpp
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

int main() {
    CURL* curl = curl_easy_init();
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8080/v1/mrd/latest/header");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    if(res == CURLE_OK) {
        json header = json::parse(response);
        std::cout << "Field strength: "
                  << header["acquisitionSystemInformation"]["systemFieldStrength_T"]
                  << " T\n";
    }

    curl_easy_cleanup(curl);
    return 0;
}
```

### JavaScript/Node.js: WebSocket Client

```javascript
const WebSocket = require('ws');
const axios = require('axios');

// WebSocket for real-time updates
const ws = new WebSocket('ws://localhost:8090/ws');

ws.on('message', (data) => {
  const msg = JSON.parse(data);
  console.log('New MRD file:', msg.filename);

  // Fetch the new file
  axios.get(`http://localhost:8080/v1/mrd/${msg.filename}`, {
    responseType: 'arraybuffer'
  }).then(response => {
    console.log('Downloaded', response.data.byteLength, 'bytes');
  });
});

ws.on('open', () => {
  console.log('Connected to MRI Marshal');
});
```

### MATLAB: Read MRI Data

```matlab
% Get MRD header
url = 'http://localhost:8080/v1/mrd/latest/header';
header = webread(url);
fprintf('Field strength: %.1f T\n', header.acquisitionSystemInformation.systemFieldStrength_T);

% Get latest ECG
ecg_url = 'http://localhost:8080/v1/bio/latest';
ecg = webread(ecg_url);
fprintf('Heart rate: %d BPM\n', ecg.heart_rate);

% Download MRD file
mrd_url = 'http://localhost:8080/v1/mrd/latest';
options = weboptions('ContentType', 'binary');
mrd_data = webread(mrd_url, options);
fid = fopen('data.mrd', 'wb');
fwrite(fid, mrd_data);
fclose(fid);
```

---

## Troubleshooting

### Connection Refused

```bash
# Check if marshals are running
docker ps | grep marshal

# Check marshal logs
docker logs cwru-mri-marshal
docker logs cwru-robot-marshal

# Test connectivity
curl -v http://localhost:8080/health
curl -v http://localhost:8081/
```

### No Data Available

```bash
# Check if clients are sending data
docker logs cwru-ecg-client
docker logs cwru-pose-client
docker logs cwru-image-streamer

# Verify data is being received
curl http://localhost:8080/v1/bio/latest
curl http://localhost:8080/v1/poses/latest
curl http://localhost:8080/v1/mrd/list
```

### WebSocket Connection Fails

```bash
# Test WebSocket with wscat
npm install -g wscat
wscat -c ws://localhost:8090/ws

# Check if port is accessible
telnet localhost 8090
```

### Firewall Issues

```bash
# Allow ports through firewall (Linux)
sudo ufw allow 8080/tcp
sudo ufw allow 8081/tcp
sudo ufw allow 8090/tcp

# Check port is listening
sudo netstat -tlnp | grep -E '8080|8081|8090'
```

### Data Format Errors

- Ensure `Content-Type: application/json` header is set for POST requests
- Verify JSON is valid using `jq` or online validators
- Check `sent_at` is Unix timestamp in milliseconds
- Ensure `client_id` is a non-empty string

### Performance Issues

- Use streaming endpoints (`/v1/bio/stream`) instead of polling `/latest`
- Use WebSocket for real-time updates instead of HTTP polling
- Download MRD files asynchronously
- Implement client-side caching for frequently accessed data

---

## Summary

**MRI Marshal** provides:
- MRD image data (ISMRMRD format)
- ECG bio signals
- Pose tracking data
- HTTP REST API + WebSocket streaming

**Robot Marshal** provides:
- Robot position and status
- Command interface
- Catheter tracking data
- Controller state
- HTTP REST API

**Base URLs:**
```
http://localhost:8080  - MRI Marshal HTTP
ws://localhost:8090    - MRI Marshal WebSocket
http://localhost:8081  - Robot Marshal HTTP
```

For more examples, see the client implementations in:
- `mri_data_marshal_worktree/clients/`
- `robot_data_marshal_worktree/client-*.cpp`
