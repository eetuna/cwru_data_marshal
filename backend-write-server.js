const express = require('express');
const axios = require('axios');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = 3001;

// Robot data marshal server address - use service name for Docker inter-container communication
// Inside Docker, services communicate using their service name from docker-compose.yml
const DATA_MARSHAL_SERVER = process.env.NODE_ENV === 'production'
  ? `http://${process.env.ROBOT_MARSHAL_HOST || 'robot-marshal'}:${process.env.ROBOT_MARSHAL_PORT || '8081'}`
  : 'http://localhost:8080';   // Local development

// MRI data marshal server address (for MRI-specific write channels)
const MRI_MARSHAL_SERVER = process.env.NODE_ENV === 'production'
  ? `http://${process.env.MRI_MARSHAL_HOST || 'mri-marshal'}:${process.env.MRI_MARSHAL_PORT || '8080'}`
  : 'http://127.0.0.1:8080';

// Load routing configuration (shared with all C++ clients)
let routesConfig = {};
try {
  const routesFile = fs.readFileSync(path.join(__dirname, 'file_routes.json'), 'utf-8');
  routesConfig = JSON.parse(routesFile);
  console.log('[Backend Write Server] Routing configuration loaded:', routesConfig);
} catch (error) {
  console.error('[Backend Write Server] Error loading routing configuration:', error);
  process.exit(1);
}

// CORS middleware - enable cross-origin requests
app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, Authorization');

  // Handle preflight requests
  if (req.method === 'OPTIONS') {
    return res.sendStatus(200);
  }
  next();
});

// Middleware
app.use(express.json());

// Helper function to get client routes
function getClientRoutes(clientId) {
  return routesConfig[clientId] || null;
}

// API endpoint: Write to data marshal server
// This endpoint directly writes to the C++ backend, no fallback to local files
app.post('/api/write/:clientId/:fileKey', async (req, res) => {
  try {
    const { clientId, fileKey } = req.params;
    const data = req.body;

    console.log(`[Backend Write Server] POST /api/write/${clientId}/${fileKey}`);
    console.log(`[Backend Write Server] Client ID: ${clientId}, File Key: ${fileKey}`);
    console.log(`[Backend Write Server] Payload:`, data);

    // Get routing configuration for this client
    const clientRoutes = getClientRoutes(clientId);
    if (!clientRoutes) {
      console.warn(`[Backend Write Server] Client '${clientId}' not found in routing config`);
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
    } else if (fileKey === '8') {
      fileName = clientRoutes.write_to9;
    } else if (fileKey === '9') {
      fileName = clientRoutes.write_to10;
    } else if (fileKey === '10') {
      fileName = clientRoutes.write_to11;
    } else if (fileKey === '11') {
      fileName = clientRoutes.write_to12;
    } else if (fileKey === '12') {
      fileName = clientRoutes.write_to13;
    } else if (fileKey === '13') {
      fileName = clientRoutes.write_to14;
    } else if (fileKey === '14') {
      fileName = clientRoutes.write_to15;
    } else if (fileKey === '15') {
      fileName = clientRoutes.write_to16;
    } else if (fileKey === '16') {
      fileName = clientRoutes.write_to17;
    } else if (fileKey === '17') {
      fileName = clientRoutes.write_to18;
    } else if (fileKey === '18') {
      fileName = clientRoutes.write_to19;
    } else if (fileKey === '19') {
      fileName = clientRoutes.write_to20;
    }  else {
      console.warn(`[Backend Write Server] Invalid fileKey: ${fileKey}`);
      return res.status(400).json({ error: `Invalid fileKey: ${fileKey}` });
    }

    if (!fileName) {
      console.warn(`[Backend Write Server] File mapping not found for fileKey: ${fileKey}`);
      return res.status(404).json({ error: `File mapping not found for fileKey: ${fileKey}` });
    }

    const mriWriteKeys = new Set([
      'slice_delta',
      'slice_delta.json',
      'file_slice_translation',
      'file_slice_translation.json',
      'file_x_rotation',
      'file_x_rotation.json',
      'file_y_rotation',
      'file_y_rotation.json',
      'file_z_rotation',
      'file_z_rotation.json',
      'file_slice_pose_transform',
      'file_slice_pose_transform.json',
      'file_slice_thickness',
      'file_slice_thickness.json',
      'slice_target'
    ]);
    const isMriWrite = mriWriteKeys.has(fileName);
    const targetServer = isMriWrite ? MRI_MARSHAL_SERVER : DATA_MARSHAL_SERVER;
    const targetFileKey = isMriWrite ? fileName.replace(/\.json$/, '') : fileName;

    console.log(`[Backend Write Server] Resolved file name: ${fileName}`);
    console.log(`[Backend Write Server] Posting to ${isMriWrite ? 'MRI' : 'Robot'} marshal at: ${targetServer}/write/${targetFileKey}`);

    // Write directly to C++ data marshal
    try {
      const response = await axios.post(`${targetServer}/write/${targetFileKey}`, data, {
        timeout: 5000
      });
      console.log(`✓ [Backend Write Server] Successfully posted data to ${isMriWrite ? 'MRI' : 'Robot'} marshal for ${fileName}`);
      console.log(`✓ [Backend Write Server] Server response:`, response.data);
      return res.json({
        success: true,
        message: `Data written to ${fileName}`,
        target_marshal: isMriWrite ? 'mri' : 'robot',
        backend_response: response.data
      });
    } catch (backendError) {
      // A marshal REJECTION (4xx with a JSON body, e.g. a clamp or a
      // left-handed slice frame) is not a connectivity failure: pass its
      // status and body through so the UI can show the actual reason.
      if (backendError.response && backendError.response.status >= 400 &&
          backendError.response.status < 500) {
        console.warn(`[Backend Write Server] Marshal rejected ${fileName}: ${backendError.response.status}`, backendError.response.data);
        return res.status(backendError.response.status).json({
          success: false,
          message: `Marshal rejected write for ${fileName}`,
          target_marshal: targetServer,
          backend_response: backendError.response.data
        });
      }
      console.error(`✗ [Backend Write Server] Failed to reach ${isMriWrite ? 'MRI' : 'Robot'} marshal at ${targetServer}/write/${targetFileKey}`);
      console.error(`✗ [Backend Write Server] Error details:`, backendError.message);

      if (backendError.response) {
        console.error(`✗ [Backend Write Server] C++ Server response status: ${backendError.response.status}`);
        console.error(`✗ [Backend Write Server] C++ Server response data:`, backendError.response.data);
      }

      return res.status(503).json({
        error: `Failed to reach ${isMriWrite ? 'MRI' : 'Robot'} data marshal server`,
        details: backendError.message,
        backend_url: `${targetServer}/write/${targetFileKey}`,
        target_marshal: isMriWrite ? 'mri' : 'robot',
        fileName: fileName
      });
    }
  } catch (error) {
    console.error('[Backend Write Server] Unexpected error:', error.message);
    res.status(500).json({
      error: 'Unexpected error on backend write server',
      details: error.message
    });
  }
});

// Health check endpoint
app.get('/health', (req, res) => {
  res.json({
    status: 'ok',
    service: 'backend-write-server',
    robot_data_marshal_server: DATA_MARSHAL_SERVER,
    mri_data_marshal_server: MRI_MARSHAL_SERVER,
    timestamp: Date.now()
  });
});

// Status endpoint - check if C++ server is reachable
app.get('/status', async (req, res) => {
  try {
    await axios.get(`${DATA_MARSHAL_SERVER}/read/file_tip_position_orientation.json`, {
      timeout: 2000
    });
    return res.json({
      status: 'connected',
      data_marshal_server: DATA_MARSHAL_SERVER,
      timestamp: Date.now()
    });
  } catch (error) {
    return res.status(503).json({
      status: 'disconnected',
      data_marshal_server: DATA_MARSHAL_SERVER,
      error: error.message,
      timestamp: Date.now()
    });
  }
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`\n[Backend Write Server] Started successfully`);
  console.log(`[Backend Write Server] Listening on http://0.0.0.0:${PORT}`);
  console.log(`[Backend Write Server] Robot Marshal: ${DATA_MARSHAL_SERVER}`);
  console.log(`[Backend Write Server] MRI Marshal: ${MRI_MARSHAL_SERVER}`);
  console.log(`[Backend Write Server] Health check: GET http://localhost:${PORT}/health`);
  console.log(`[Backend Write Server] Status check: GET http://localhost:${PORT}/status\n`);
});
