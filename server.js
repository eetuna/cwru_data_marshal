const express = require('express');
const axios = require('axios');
const path = require('path');
const fs = require('fs');

const app = express();
const PORT = 3000;

const CLIENT_ID = 'client-webgl';

// Data marshal server address - use service name for Docker inter-container communication
// Inside Docker, services communicate using their service name from docker-compose.yml
const DATA_MARSHAL_SERVER = process.env.NODE_ENV === 'production'
  ? 'http://server:8080'       // Docker service name
  : 'http://localhost:8080';   // Local development

// Streaming server address (will later be changed to point to the MRI data marshal)
const STREAMING_SERVER = process.env.NODE_ENV === 'production'
  ? 'http://streaming-server:8081'  // Docker service name
  : 'http://127.0.0.1:8081';        // Local development

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

// Helper function to load fallback data from the files directory
function loadFallbackData(fileName) {
  try {
    const filePath = path.join(__dirname, 'files', fileName);
    if (fs.existsSync(filePath)) {
      const data = fs.readFileSync(filePath, 'utf-8');
      console.log(`Loaded fallback data for '${fileName}' from: ${filePath}`);
      return JSON.parse(data);
    }
  } catch (error) {
    console.error(`Error loading fallback data for '${fileName}':`, error);
  }
  return null;
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
    } else {
      return res.status(400).json({ error: `Invalid fileKey: ${fileKey}` });
    }

    if (!fileName) {
      return res.status(404).json({ error: `File mapping not found for fileKey: ${fileKey}` });
    }

    // For streaming 2D images (read_from / fileKey 0), try streaming-server first
    if (fileName === clientRoutes.read_from) {
      try {
        const response = await axios.get(`${STREAMING_SERVER}/read/${fileName}`);
        console.log(`Proxied ${fileName} from streaming-server`);
        return res.json(response.data);
      } catch (streamError) {
        console.warn(`Streaming server unavailable for ${fileName}, falling back: ${streamError.message}`);
        const fallback = loadFallbackData(fileName);
        if (fallback) {
          fallback.timestamp = Date.now();
          return res.json(fallback);
        }
        return res.status(404).json({ error: `Unable to load ${fileName} from streaming server or files/` });
      }
    }

    // For 3D images (read_from2 / fileKey 1), try streaming-server first
    if (fileName === clientRoutes.read_from2) {
      try {
        const response = await axios.get(`${STREAMING_SERVER}/read/${fileName}`);
        console.log(`Proxied ${fileName} from streaming-server`);
        return res.json(response.data);
      } catch (streamError) {
        console.warn(`Streaming server unavailable for ${fileName}, falling back: ${streamError.message}`);
        const fallback = loadFallbackData(fileName);
        if (fallback) {
          fallback.timestamp = Date.now();
          return res.json(fallback);
        }
        return res.status(404).json({ error: `Unable to load ${fileName} from streaming server or files/` });
      }
    }

    // For all other reads, go directly to the C++ data marshal
    try {
      const response = await axios.get(`${DATA_MARSHAL_SERVER}/read/${fileName}`);
      console.log(`Fetched ${fileName} from C++ data marshal`);
      return res.json(response.data);
    } catch (backendError) {
      console.warn(`C++ data marshal unavailable, falling back to files/: ${backendError.message}`);
      const fallback = loadFallbackData(fileName);
      if (fallback) {
        fallback.timestamp = Date.now();
        console.log(`Served fallback ${fileName} from files/`);
        return res.json(fallback);
      }
      return res.status(503).json({
        error: 'Data marshal server unavailable and no fallback data found in files/',
        details: `Backend error: ${backendError.message}`,
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
      console.warn(`C++ data marshal unavailable, saving to local file instead: ${backendError.message}`);

      // Fallback: save to local files directory
      try {
        const filePath = path.join(__dirname, 'files', fileName);
        const fileData = {
          ...data,
          timestamp: Date.now(),
          _source: 'local_fallback'
        };
        fs.writeFileSync(filePath, JSON.stringify(fileData, null, 2));
        console.log(`✓ Saved data to local file: ${filePath}`);
        return res.json({ success: true, message: `Data written to local file (backend unavailable)`, fileName: fileName });
      } catch (fileError) {
        console.error(`Failed to save to local file: ${fileError.message}`);
        return res.status(500).json({ error: 'Failed to write data - backend and local file save both failed', details: fileError.message });
      }
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
  console.log(`Connected to Data Marshal Server at ${DATA_MARSHAL_SERVER}`);
  console.log(`Streaming Server at ${STREAMING_SERVER}`);
});
