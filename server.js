const express = require('express');
const axios = require('axios');
const path = require('path');
const fs = require('fs');
const { execFile } = require('child_process');

const app = express();
const PORT = 3000;

const CLIENT_ID = 'client-webgl';

// Robot data marshal server address
const DATA_MARSHAL_SERVER = process.env.NODE_ENV === 'production'
  ? `http://${process.env.ROBOT_MARSHAL_HOST || 'robot-marshal'}:${process.env.ROBOT_MARSHAL_PORT || '8081'}`
  : 'http://localhost:8080';

// MRI Marshal address (serves frame metadata; HDF5 files read from shared /session-data volume)
const MRI_MARSHAL_SERVER = process.env.NODE_ENV === 'production'
  ? `http://${process.env.MRI_MARSHAL_HOST || 'mri-marshal'}:${process.env.MRI_MARSHAL_PORT || '8080'}`
  : 'http://127.0.0.1:8080';

// Path to the Python HDF5 reader script (co-located with server.js)
const HDF5_READER = path.join(__dirname, 'read_hdf5.py');

// Writable metrics log path inside the container
const HDF5_READ_METRICS_FILE = process.env.HDF5_READ_METRICS_FILE || '/tmp/webgl-client-hdf5-read-metrics.jsonl';

async function appendHdf5ReadMetric(entry) {
  try {
    await fs.promises.appendFile(HDF5_READ_METRICS_FILE, `${JSON.stringify(entry)}\n`);
  } catch (error) {
    console.error(`Failed to append HDF5 read metric: ${error.message}`);
  }
}

// ============================================================================
// HDF5 reader using Python h5py (mirrors viz_client_main.cpp logic)
//
// viz_client flow:
//   1. GET /image/latest  -> { path, error }  (marshal v2 API; live mode only.
//      Returns 204 No Content when no image yet, 404 in dump mode. The old
//      /v1/mrd/latest route with frame_index/dims was removed in the v2 rewrite,
//      so frame_index defaults to 0 for the single-frame latest companion.)
//   2. Open HDF5 file at path in SWMR read mode
//   3. Read /images/data dataset  shape [frames, channels, z, y, x] float32
//   4. Extract middle slice (2D) or full volume (3D) for the given frame
//
// We replicate that here by spawning read_hdf5.py with h5py.
// ============================================================================

/**
 * Fetch latest frame metadata from MRI Marshal.
 */
async function fetchMriLatest() {
  const response = await axios.get(`${MRI_MARSHAL_SERVER}/image/latest`, {
    timeout: 2000,
    // Don't throw on 404 (dump mode); treat it as "no data" like an empty 204.
    validateStatus: (s) => (s >= 200 && s < 300) || s === 404,
  });
  const data = response.data;
  // 204 No Content (no image yet) or 404 (dump mode) -> empty/non-object body.
  if (!data || typeof data !== 'object') {
    return { path: null, frame_index: 0, dims: {}, timestamp: Date.now() };
  }
  const meta = data.data || data;
  return {
    path: meta.path,
    // v2 /image/latest exposes a single latest image and no frame_index/dims.
    frame_index: meta.frame_index ?? 0,
    dims: meta.dims || {},
    timestamp: meta.ts || meta.timestamp || Date.now(),
  };
}

/**
 * Read frame data from HDF5 by spawning the Python reader.
 * mode: "2d" (middle slice) or "3d" (full volume)
 * Returns parsed JSON: { width, height, [depth], values: number[] }
 */
function readHdf5Frame(filePath, frameIndex, mode) {
  return new Promise((resolve, reject) => {
    execFile('python3', [HDF5_READER, filePath, String(frameIndex), mode],
      { maxBuffer: 50 * 1024 * 1024, env: { ...process.env, HDF5_USE_FILE_LOCKING: 'FALSE' } },
      (error, stdout, stderr) => {
        if (error) {
          return reject(new Error(`h5py reader failed: ${error.message} ${stderr}`));
        }
        try {
          const result = JSON.parse(stdout);
          if (result.error) return reject(new Error(result.error));
          resolve(result);
        } catch (e) {
          reject(new Error(`Failed to parse h5py output: ${e.message}`));
        }
      }
    );
  });
}

// ============================================================================
// Express app
// ============================================================================

// CORS middleware - enable cross-origin requests
app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, Authorization');

  if (req.method === 'OPTIONS') {
    return res.sendStatus(200);
  }
  next();
});

// Middleware
app.use(express.json());
app.use(express.static(__dirname));

// Load routing configuration (shared with all C++ clients)
let routesConfig = {};
try {
  const routesFile = fs.readFileSync(path.join(__dirname, 'file_routes.json'), 'utf-8');
  routesConfig = JSON.parse(routesFile);
  console.log('Routing configuration loaded:', routesConfig);
} catch (error) {
  console.error('Error loading routing configuration:', error);
  process.exit(1);
}

// Helper function to get client routes
function getClientRoutes(clientId) {
  return routesConfig[clientId] || null;
}

// API endpoint: Read from data marshal server
app.get('/api/read/:clientId/:fileKey', async (req, res) => {
  try {
    const { clientId, fileKey } = req.params;

    // Get routing configuration for this client
    const clientRoutes = getClientRoutes(clientId);
    if (!clientRoutes) {
      return res.status(404).json({ error: `Client '${clientId}' not found in routing config` });
    }

    // Map fileKey to actual file name from routing config
    let fileName = null;
    if (fileKey === '0') {
      fileName = clientRoutes.read_from;
    } else if (fileKey === '1') {
      fileName = clientRoutes.read_from2;
    } else if (fileKey === '2') {
      fileName = clientRoutes.read_from3;
    } else if (fileKey === '3') {
      fileName = clientRoutes.read_from4;
    } else if (fileKey === '4') {
      fileName = clientRoutes.read_from5;
    } else {
      return res.status(400).json({ error: `Invalid fileKey: ${fileKey}` });
    }

    if (!fileName) {
      return res.status(404).json({ error: `File mapping not found for fileKey: ${fileKey}` });
    }

    // ------------------------------------------------------------------
    // 2D streaming images: MRI Marshal metadata + h5py middle-slice read
    // ------------------------------------------------------------------
    if (fileName === 'file_streaming_2D_images.json') {
      try {
        const readStartedAtMs = Date.now();
        const meta = await fetchMriLatest();
        if (!meta.path) {
          return res.status(503).json({ error: 'No MRI data available yet' });
        }
        const hdf5ReadStartedAtMs = Date.now();
        const sliceData = await readHdf5Frame(meta.path, meta.frame_index, '2d');
        const readEndedAtMs = Date.now();
        const metadataDurationMs = hdf5ReadStartedAtMs - readStartedAtMs;
        const hdf5ReadDurationMs = readEndedAtMs - hdf5ReadStartedAtMs;
        sliceData.timestamp = meta.timestamp;
        sliceData.frame_index = meta.frame_index;
        sliceData.sent_from_serverjs = Date.now();
        sliceData.metadata_duration_ms = metadataDurationMs;
        sliceData.hdf5_read_duration_ms = hdf5ReadDurationMs;
        console.log(`[2D] frame ${meta.frame_index} -> ${sliceData.width}x${sliceData.height} from ${path.basename(meta.path)}`);
        appendHdf5ReadMetric({
          ts: new Date().toISOString(),
          frame_index: meta.frame_index,
          file: path.basename(meta.path),
          read_started_at_ms: readStartedAtMs,
          hdf5_read_started_at_ms: hdf5ReadStartedAtMs,
          read_ended_at_ms: readEndedAtMs,
          metadata_duration_ms: metadataDurationMs,
          hdf5_read_duration_ms: hdf5ReadDurationMs,
          total_duration_ms: readEndedAtMs - readStartedAtMs
        });
        return res.json(sliceData);
      } catch (err) {
        console.error(`[2D] MRI read error: ${err.message}`);
        return res.status(503).json({ error: 'MRI image read failed', details: err.message, fileName });
      }
    }

    // ------------------------------------------------------------------
    // 3D volume images: MRI Marshal metadata + h5py full-volume read
    // ------------------------------------------------------------------
    if (fileName === 'file_3D_images.json') {
      try {
        const meta = await fetchMriLatest();
        if (!meta.path) {
          return res.status(503).json({ error: 'No MRI data available yet' });
        }
        const volumeData = await readHdf5Frame(meta.path, meta.frame_index, '3d');
        volumeData.timestamp = meta.timestamp;
        console.log(`[3D] frame ${meta.frame_index} -> ${volumeData.width}x${volumeData.height}x${volumeData.depth} from ${path.basename(meta.path)}`);
        return res.json(volumeData);
      } catch (err) {
        console.error(`[3D] MRI read error: ${err.message}`);
        return res.status(503).json({ error: 'MRI volume read failed', details: err.message, fileName });
      }
    }

    // ------------------------------------------------------------------
    // All other reads: Robot data marshal
    // ------------------------------------------------------------------
    try {
      const response = await axios.get(`${DATA_MARSHAL_SERVER}/read/${fileName}`);
      console.log(`Fetched ${fileName} from C++ data marshal`);
      return res.json(response.data);
    } catch (backendError) {
      console.error(`C++ data marshal unavailable for ${fileName}: ${backendError.message}`);
      return res.status(503).json({
        error: 'Data marshal server unavailable',
        details: backendError.message,
        fileName: fileName
      });
    }
  } catch (error) {
    console.error('Error reading from server:', error.message);
    res.status(500).json({ error: 'Failed to read from data marshal server', details: error.message });
  }
});

// API endpoint: Write to data marshal server
app.post('/api/write/:clientId/:fileKey', async (req, res) => {
  try {
    const { clientId, fileKey } = req.params;
    const data = req.body;

    // Get routing configuration for this client
    const clientRoutes = getClientRoutes(clientId);
    if (!clientRoutes) {
      return res.status(404).json({ error: `Client '${clientId}' not found in routing config` });
    }

    // Map fileKey to actual file name from routing config
    let fileName = null;
    if (fileKey === '0') {
      fileName = clientRoutes.write_to;
    } else if (fileKey === '1') {
      fileName = clientRoutes.write_to2;
    } else if (fileKey === '2') {
      fileName = clientRoutes.write_to3;
    } else if (fileKey === '3') {
      fileName = clientRoutes.write_to4;
    } else if (fileKey === '4') {
      fileName = clientRoutes.write_to5;
    } else if (fileKey === '5') {
      fileName = clientRoutes.write_to6;
    } else if (fileKey === '6') {
      fileName = clientRoutes.write_to7;
    } else if (fileKey === '7') {
      fileName = clientRoutes.write_to8;
    } else {
      return res.status(400).json({ error: `Invalid fileKey: ${fileKey}` });
    }

    if (!fileName) {
      return res.status(404).json({ error: `File mapping not found for fileKey: ${fileKey}` });
    }

    // Write directly to C++ data marshal
    try {
      const response = await axios.post(`${DATA_MARSHAL_SERVER}/write/${fileName}`, data);
      console.log(`✓ Posted data to C++ data marshal for ${fileName}`);
      return res.json({ success: true, message: `Data written to ${fileName}`, data: response.data });
    } catch (backendError) {
      console.error(`C++ data marshal unavailable for write to ${fileName}: ${backendError.message}`);
      return res.status(503).json({
        error: 'Data marshal server unavailable',
        details: backendError.message,
        fileName: fileName
      });
    }
  } catch (error) {
    console.error('Error writing to server:', error.message);
    res.status(500).json({ error: 'Failed to write to data marshal server', details: error.message });
  }
});

// Serve index.html for root path
app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

app.listen(PORT, () => {
  console.log(`WebGL Frontend Server running at http://localhost:${PORT}`);
  console.log(`Connected to Robot Data Marshal at ${DATA_MARSHAL_SERVER}`);
  console.log(`Connected to MRI Data Marshal at ${MRI_MARSHAL_SERVER}`);
  console.log(`2D/3D images: MRI Marshal metadata + h5py HDF5 reader`);
});
