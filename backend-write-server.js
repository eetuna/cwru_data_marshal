const express = require('express');
const axios = require('axios');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = 3001;

// Data marshal server address - use service name for Docker inter-container communication
// Inside Docker, services communicate using their service name from docker-compose.yml
const DATA_MARSHAL_SERVER = process.env.NODE_ENV === 'production'
  ? `http://${process.env.ROBOT_MARSHAL_HOST || 'robot-marshal'}:${process.env.ROBOT_MARSHAL_PORT || '8081'}`
  : 'http://localhost:8080';   // Local development

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
    }  else {
      console.warn(`[Backend Write Server] Invalid fileKey: ${fileKey}`);
      return res.status(400).json({ error: `Invalid fileKey: ${fileKey}` });
    }

    if (!fileName) {
      console.warn(`[Backend Write Server] File mapping not found for fileKey: ${fileKey}`);
      return res.status(404).json({ error: `File mapping not found for fileKey: ${fileKey}` });
    }

    console.log(`[Backend Write Server] Resolved file name: ${fileName}`);
    console.log(`[Backend Write Server] Posting to C++ server at: ${DATA_MARSHAL_SERVER}/write/${fileName}`);

    // Write directly to C++ data marshal
    try {
      const response = await axios.post(`${DATA_MARSHAL_SERVER}/write/${fileName}`, data, {
        timeout: 5000
      });
      console.log(`✓ [Backend Write Server] Successfully posted data to C++ server for ${fileName}`);
      console.log(`✓ [Backend Write Server] Server response:`, response.data);
      return res.json({
        success: true,
        message: `Data written to ${fileName}`,
        backend_response: response.data
      });
    } catch (backendError) {
      console.error(`✗ [Backend Write Server] Failed to reach C++ server at ${DATA_MARSHAL_SERVER}/write/${fileName}`);
      console.error(`✗ [Backend Write Server] Error details:`, backendError.message);

      if (backendError.response) {
        console.error(`✗ [Backend Write Server] C++ Server response status: ${backendError.response.status}`);
        console.error(`✗ [Backend Write Server] C++ Server response data:`, backendError.response.data);
      }

      return res.status(503).json({
        error: 'Failed to reach C++ data marshal server',
        details: backendError.message,
        backend_url: `${DATA_MARSHAL_SERVER}/write/${fileName}`,
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
    data_marshal_server: DATA_MARSHAL_SERVER,
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
  console.log(`[Backend Write Server] Connected to C++ Data Marshal Server at: ${DATA_MARSHAL_SERVER}`);
  console.log(`[Backend Write Server] Health check: GET http://localhost:${PORT}/health`);
  console.log(`[Backend Write Server] Status check: GET http://localhost:${PORT}/status\n`);
});
