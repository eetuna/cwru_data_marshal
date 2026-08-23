const express = require('express');
const axios = require('axios');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

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
    // Publish generation: the marshal bumps this on every snapshot publish,
    // so it is the "has anything changed?" signal. Null on older marshals.
    generation: meta.generation ?? null,
    timestamp: meta.ts || meta.timestamp || Date.now(),
  };
}

// The `timestamp` the browser uses to decide whether to re-render
// (webgl-demo.js gates on `data.timestamp !== lastTimestamp`). With a
// generation-aware marshal this is derived from the generation — stable
// between publishes, so idle polls stop triggering re-renders (the
// post-scan infinite-rerender / plane-jitter bug). Older marshals without
// a generation fall back to the old wall-clock behavior.
//
// Restart safety: the marshal's generation restarts at 0 when the marshal
// restarts, and the snapshot path is constant, so a bare generation could
// repeat values a long-lived viewer has already seen (stale cache hits,
// silently skipped frames). Detect a restart as a generation decrease and
// fold a nonce into the token so post-restart values never collide with
// pre-restart ones.
let maxGenerationSeen = -1;
let marshalRestartNonce = 0;

function noteGeneration(meta) {
  if (meta.generation === null) return;
  if (meta.generation < maxGenerationSeen) {
    marshalRestartNonce += 1;
    mriReadCache['2d'] = null;
    mriReadCache['3d'] = null;
    console.log(`[mri] marshal restart detected (generation ${meta.generation} < ${maxGenerationSeen}); caches cleared`);
  }
  maxGenerationSeen = meta.generation;
}

function changeToken(meta) {
  return meta.generation
    ? `${marshalRestartNonce}:${meta.generation}`
    : meta.timestamp;
}

// Last successfully served body per mode, keyed by (generation, path). On a
// cache hit the snapshot is unchanged since the last read — return the same
// body without spawning an h5py read. noteGeneration() clears both caches
// on a detected marshal restart.
const mriReadCache = { '2d': null, '3d': null };

function cachedMriBody(mode, meta) {
  noteGeneration(meta);
  const c = mriReadCache[mode];
  if (c && meta.generation !== null &&
      c.generation === meta.generation && c.path === meta.path) {
    return c.body;
  }
  return null;
}

/**
 * Read frame data from HDF5 via a persistent Python worker (read_hdf5.py --serve).
 * A per-poll `python3` spawn costs ~300-400 ms (interpreter start), capping the
 * viewer at ~2-3 fps; the warm worker answers in ~10 ms. One JSON request per
 * stdin line -> one JSON response per stdout line, served in order.
 * mode: "2d" (middle slice) or "3d" (full volume)
 * Returns parsed JSON: { width, height, [depth], values: number[] }
 */
let hdf5Proc = null;
let hdf5Pending = [];   // FIFO of {resolve, reject}; worker replies in order
let hdf5Buf = '';

function ensureHdf5Worker() {
  if (hdf5Proc) return;
  hdf5Proc = spawn('python3', [HDF5_READER, '--serve'],
    { env: { ...process.env, HDF5_USE_FILE_LOCKING: 'FALSE' } });
  hdf5Buf = '';
  hdf5Proc.stdout.on('data', (chunk) => {
    hdf5Buf += chunk.toString();
    let nl;
    while ((nl = hdf5Buf.indexOf('\n')) >= 0) {
      const line = hdf5Buf.slice(0, nl);
      hdf5Buf = hdf5Buf.slice(nl + 1);
      const pending = hdf5Pending.shift();
      if (!pending) continue;
      try {
        const result = JSON.parse(line);
        if (result.error) pending.reject(new Error(result.error));
        else pending.resolve(result);
      } catch (e) {
        pending.reject(new Error(`Failed to parse h5py output: ${e.message}`));
      }
    }
  });
  hdf5Proc.stderr.on('data', (d) => console.error('[read_hdf5]', d.toString().trim()));
  hdf5Proc.on('exit', (code) => {
    console.error(`[read_hdf5] worker exited (code ${code}); will respawn on next read`);
    hdf5Proc = null;
    const stranded = hdf5Pending;
    hdf5Pending = [];
    stranded.forEach((p) => p.reject(new Error('h5py worker exited')));
  });
}

function readHdf5Frame(filePath, frameIndex, mode) {
  return new Promise((resolve, reject) => {
    ensureHdf5Worker();
    hdf5Pending.push({ resolve, reject });
    hdf5Proc.stdin.write(JSON.stringify({ path: filePath, frame: frameIndex, mode }) + '\n');
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
  // Never cache API reads — /api/read/.../0 returns a new frame each poll but at a
  // stable URL, so browser caching would freeze the viewer on the first frame.
  res.header('Cache-Control', 'no-store, no-cache, must-revalidate');

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
        const cached2d = cachedMriBody('2d', meta);
        if (cached2d) {
          return res.json(cached2d);
        }
        const hdf5ReadStartedAtMs = Date.now();
        const sliceData = await readHdf5Frame(meta.path, meta.frame_index, '2d');
        const readEndedAtMs = Date.now();
        const metadataDurationMs = hdf5ReadStartedAtMs - readStartedAtMs;
        const hdf5ReadDurationMs = readEndedAtMs - hdf5ReadStartedAtMs;
        sliceData.timestamp = changeToken(meta);
        sliceData.generation = meta.generation;
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
        mriReadCache['2d'] = { generation: meta.generation, path: meta.path, body: sliceData };
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
        const cached3d = cachedMriBody('3d', meta);
        if (cached3d) {
          return res.json(cached3d);
        }
        const volumeData = await readHdf5Frame(meta.path, meta.frame_index, '3d');
        volumeData.timestamp = changeToken(meta);
        volumeData.generation = meta.generation;
        console.log(`[3D] frame ${meta.frame_index} -> ${volumeData.width}x${volumeData.height}x${volumeData.depth} from ${path.basename(meta.path)}`);
        mriReadCache['3d'] = { generation: meta.generation, path: meta.path, body: volumeData };
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
