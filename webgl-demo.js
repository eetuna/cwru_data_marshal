// WebGL rendering engine - renders 3D volume with separate 2D image canvas
import { Image2DRenderer } from "./render-2d-image.js";

let deltaTime = 0;
let mouseRotationX = 0.0;
let mouseRotationY = 0.0;
let mouseRotationZ = 0.0;
let isMouseDown = false;
let lastMouseX = 0;
let lastMouseY = 0;

main();

async function main() {
  const canvas3D = document.querySelector("#glcanvas");
  const canvas2D = document.querySelector("#canvas2d");
  const canvasFK = document.querySelector("#canvasFK");
  const gl = canvas3D.getContext("webgl");
  const gl2d = canvas2D.getContext("2d");
  const glFK = canvasFK.getContext("webgl");

  if (gl === null) {
    alert("Unable to initialize WebGL. Your browser or machine may not support it.");
    return;
  }

  if (glFK === null) {
    alert("Unable to initialize WebGL for FK canvas.");
    return;
  }

  // FK canvas mouse rotation state (independent from 3D volume)
  let fkMouseRotationX = -0.5;
  let fkMouseRotationY = 0.3;
  let fkMouseRotationZ = 0.0;
  let fkIsMouseDown = false;
  let fkLastMouseX = 0;
  let fkLastMouseY = 0;

  // Forward kinematics control points: array of {x, y, z}
  let fkControlPoints = [];

  const updateStatus = (id, msg) => {
    const el = document.getElementById(id);
    if (el) el.textContent = msg;
  };
  
  updateStatus('status', '✓ WebGL initialized');

  gl.clearColor(0.0, 0.0, 0.0, 1.0);
  gl.clear(gl.COLOR_BUFFER_BIT);

  // 3D volume shader
  const vsSource = `
    attribute vec4 aVertexPosition;
    attribute vec2 aTextureCoord;
    uniform mat4 uModelViewMatrix;
    uniform mat4 uProjectionMatrix;
    varying highp vec2 vTextureCoord;
    void main(void) {
      gl_Position = uProjectionMatrix * uModelViewMatrix * aVertexPosition;
      vTextureCoord = aTextureCoord;
    }
  `;

  const fsSource = `
    precision mediump float;
    varying highp vec2 vTextureCoord;
    uniform sampler2D uSampler;
    void main(void) {
      vec4 texColor = texture2D(uSampler, vTextureCoord);
      gl_FragColor = vec4(texColor.r, texColor.r, texColor.r, texColor.r);
    }
  `;

  const shaderProgram = initShaderProgram(gl, vsSource, fsSource);
  const programInfo = {
    program: shaderProgram,
    attribLocations: {
      vertexPosition: gl.getAttribLocation(shaderProgram, "aVertexPosition"),
      textureCoord: gl.getAttribLocation(shaderProgram, "aTextureCoord"),
    },
    uniformLocations: {
      projectionMatrix: gl.getUniformLocation(shaderProgram, "uProjectionMatrix"),
      modelViewMatrix: gl.getUniformLocation(shaderProgram, "uModelViewMatrix"),
      uSampler: gl.getUniformLocation(shaderProgram, "uSampler"),
    },
  };

  const readServerUrl = "http://localhost:3000";    // Main server (handles routing and fallbacks)
  const writeServerUrl = "http://localhost:3001";   // Backend write server (for writing data)
  const clientId = "client-webgl";
  
  let volumeSlices = null;
  let currentVolumeData = null;
  let currentImageData = null;
  let lastTimestamp = -1;
  let lastVolumeTimestamp = -1;
  let lastTipTimestamp = -1;
  let lastFKTimestamp = -1;

  // Create 2D image renderer
  const image2DRenderer = new Image2DRenderer(gl);

  // Create volume slices from 3D data
  function createVolumeSlices(volumeData, maxSlices = 16) {
    const { width, height, depth, values, step } = volumeData;
    const slices = [];
    const sliceStep = step || Math.max(1, Math.floor(depth / maxSlices));
    
    for (let z = 0; z < depth; z += sliceStep) {
      const slice = new Float32Array(width * height);
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          const volumeIndex = z * (width * height) + y * width + x;
          const sliceIndex = y * width + x;
          slice[sliceIndex] = values[volumeIndex];
        }
      }
      
      slices.push({
        data: slice,
        z: (z / depth) * 2 - 1,
        width: width,
        height: height
      });
    }
    
    return slices;
  }

  // Render a textured quad
  function renderQuad(gl, programInfo, texture, projectionMatrix, modelViewMatrix) {
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.uniform1i(programInfo.uniformLocations.uSampler, 0);
    
    gl.useProgram(programInfo.program);
    gl.uniformMatrix4fv(programInfo.uniformLocations.projectionMatrix, false, projectionMatrix);
    gl.uniformMatrix4fv(programInfo.uniformLocations.modelViewMatrix, false, modelViewMatrix);
    
    const quadPositions = [
      -1.0, -1.0, 0.0,
       1.0, -1.0, 0.0,
       1.0,  1.0, 0.0,
      -1.0,  1.0, 0.0,
    ];
    
    const quadTexCoords = [
      0.0, 0.0,
      1.0, 0.0,
      1.0, 1.0,
      0.0, 1.0,
    ];
    
    const quadIndices = [0, 1, 2, 0, 2, 3];
    
    const posBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(quadPositions), gl.STATIC_DRAW);
    gl.vertexAttribPointer(programInfo.attribLocations.vertexPosition, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.vertexPosition);
    
    const texBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, texBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(quadTexCoords), gl.STATIC_DRAW);
    gl.vertexAttribPointer(programInfo.attribLocations.textureCoord, 2, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.textureCoord);
    
    const idxBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(quadIndices), gl.STATIC_DRAW);
    
    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
  }

  // Render 2D image on 2D canvas using simple 2D context
  function render2DImage(ctx, imageData) {
    if (!imageData || !imageData.values) return;

    const { width, height, values } = imageData;
    
    // Resize canvas to match image dimensions
    ctx.canvas.width = width;
    ctx.canvas.height = height;

    // Find min/max for normalization
    let minValue = Infinity;
    let maxValue = -Infinity;
    for (let i = 0; i < values.length; i++) {
      if (values[i] < minValue) minValue = values[i];
      if (values[i] > maxValue) maxValue = values[i];
    }
    
    const range = maxValue - minValue > 0.001 ? maxValue - minValue : 1.0;

    // Create image data
    const imageDataObj = ctx.createImageData(width, height);
    const data = imageDataObj.data;

    for (let i = 0; i < values.length; i++) {
      const normalized = (values[i] - minValue) / range;
      const value = Math.min(1.0, Math.max(0.0, normalized));
      const byteValue = Math.round(value * 255);
      
      data[i * 4] = byteValue;     // R
      data[i * 4 + 1] = byteValue; // G
      data[i * 4 + 2] = byteValue; // B
      data[i * 4 + 3] = 255;       // A
    }

    ctx.putImageData(imageDataObj, 0, 0);
    // console.log(`[2D Image] Rendered ${width}x${height}`);
  }

  // Get pixel from 2D image click
  function getPixelFromImage2DClick(screenX, screenY, canvas, imageData) {
    if (!imageData || !imageData.values) return null;

    const { width, height, values } = imageData;

    // Map screen coordinates to image pixels
    // Use clientWidth/clientHeight (displayed size) not canvas.width/height (internal resolution)
    const pixelX = Math.floor((screenX / canvas.clientWidth) * width);
    const pixelY = Math.floor((screenY / canvas.clientHeight) * height);

    // Clamp to valid range
    const x = Math.max(0, Math.min(width - 1, pixelX));
    const y = Math.max(0, Math.min(height - 1, pixelY));

    const index = y * width + x;
    const pixelValue = values[index];

    // console.log(`[2D Click] Screen (${screenX.toFixed(0)}, ${screenY.toFixed(0)}) -> Image pixel [${x}, ${y}], value=${pixelValue.toFixed(4)}`);

    return {
      pixelX: x,
      pixelY: y,
      value: pixelValue
    };
  }

  // Handle 2D canvas clicks
  function handleCanvas2DClick(mouseX, mouseY) {
    console.log(`[CLICK] 2D canvas click at (${mouseX.toFixed(0)}, ${mouseY.toFixed(0)})`);

    const pixelHit = getPixelFromImage2DClick(mouseX, mouseY, canvas2D, currentImageData);
    if (pixelHit) {
      updateStatus('selectedPoint', `[${pixelHit.pixelX}, ${pixelHit.pixelY}]`);
      updateStatus('pointValue', `${(pixelHit.value * 255).toFixed(0)} / 255`);
      console.log(`✓ Selected pixel [${pixelHit.pixelX}, ${pixelHit.pixelY}], value=${pixelHit.value.toFixed(4)}`);
      
      // Send pixel coordinates to C++ server
      postPixelCoordinatesToServer(pixelHit.pixelX, pixelHit.pixelY);
    }
  }

  // POST pixel coordinates to C++ server at tip_position_orientation endpoint
  async function postPixelCoordinatesToServer(pixelX, pixelY) {
    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        values: [pixelX, pixelY, 0]  // z=0 for 2D image plane
      };
      
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/0`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(payload)
      });
      
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      
      const result = await response.json();
      console.log(`✓ Pixel coordinates posted to server:`, payload);
      console.log(`Server response:`, result);
      updateStatus('debug', `Sent pixel [${pixelX}, ${pixelY}] to server`);
    } catch (error) {
      console.error('Error posting pixel coordinates:', error);
      updateStatus('debug', `✗ Failed to post pixels: ${error.message}`);
    }
  }

  // Fetch 2D image from server
  async function updateTextureFromServer() {
    try {
      const response = await fetch(`${readServerUrl}/api/read/${clientId}/0`);
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      
      const data = await response.json();
      
      if (data.timestamp && data.timestamp !== lastTimestamp) {
        lastTimestamp = data.timestamp;
        
        if (data.values && Array.isArray(data.values)) {
          currentImageData = data;
          render2DImage(gl2d, data);
          
          // Extract metadata and update renderer
          const metadata = {
            position: data.position,
            orientation: data.orientation,
            pixelSize: data.pixelSize
          };
          image2DRenderer.updateImage(data.values, data.width, data.height, metadata);
          
          updateStatus('status2d', `✓ ${data.width}x${data.height}`);
          return true;
        }
      }
    } catch (error) {
      console.error('Error loading 2D image:', error);
      updateStatus('status2d', `✗ ${error.message}`);
    }
    return false;
  }

  // Fetch tip position/orientation from server (read_from3 = fileKey 2)
  async function updateTipPoseFromServer() {
    try {
      const response = await fetch(`${readServerUrl}/api/read/${clientId}/2`);
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      const data = await response.json();
      const ts = data.sent_at || data.received_at || data.timestamp || 0;

      if (ts && ts !== lastTipTimestamp) {
        lastTipTimestamp = ts;

        if (data.values && Array.isArray(data.values)) {
          const v = data.values;
          const posStr = v.length >= 3
            ? `Pos (${v[0].toFixed(2)}, ${v[1].toFixed(2)}, ${v[2].toFixed(2)})`
            : `Values: [${v.join(", ")}]`;
          const oriStr = v.length >= 6
            ? ` | Ori (${v[3].toFixed(2)}, ${v[4].toFixed(2)}, ${v[5].toFixed(2)})`
            : "";
          const extra = v.length >= 7 ? ` | \u03BA=${v[6].toFixed(2)}` : "";
          updateStatus("tipPose", posStr + oriStr + extra);
        }
      }
    } catch (error) {
      // Silently retry - tip data may not be available yet
    }
  }



  // Parse flat forward kinematics values into control points
  // Input: [v1,v2,v3, v4,v5,v6, v7,v8,v9, ...]
  // Output: [{x:v1,y:v2,z:v3}, {x:v4,y:v5,z:v6}, {x:v7,y:v8,z:v9}, ...]
  // Handles arbitrary number of control points (length / 3)
  function parseFKControlPoints(values) {
    const points = [];
    const numPoints = Math.floor(values.length / 3);
    for (let i = 0; i < numPoints; i++) {
      points.push({
        x: values[i * 3],
        y: values[i * 3 + 1],
        z: values[i * 3 + 2]
      });
    }
    return points;
  }

  // Fetch forward kinematics from server (read_from4 = fileKey 3)
  async function updateForwardKinematicsFromServer() {
    try {
      const response = await fetch(`${readServerUrl}/api/read/${clientId}/3`);

      const data = await response.json();
      const ts = data.sent_at || data.received_at || data.timestamp || 0;

      if (ts && ts !== lastFKTimestamp) {
        lastFKTimestamp = ts;

        if (data.values && Array.isArray(data.values)) {
          const v = data.values;
          const valuesStr = v.map(val => val.toFixed(2)).join(", ");
          updateStatus("fwdKinematics", `[${valuesStr}]`);

          // Parse values into control points (every 3 values = one x,y,z point)
          fkControlPoints = parseFKControlPoints(v);
          const numPts = fkControlPoints.length;
          const ptsStr = fkControlPoints.map((p, i) =>
            `P${i+1}(${p.x.toFixed(2)}, ${p.y.toFixed(2)}, ${p.z.toFixed(2)})`
          ).join(" | ");
          updateStatus("fkCtrlPts", `${numPts} points: ${ptsStr}`);
        }
      }
    } catch (error) {
      // Silently retry - forward kinematics data may not be available yet
    }
  }

  async function updateVolumeFromServer() {
    try {
      const response = await fetch(`${readServerUrl}/api/read/${clientId}/1`);
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      
      const data = await response.json();
      
      if (data.timestamp && data.timestamp !== lastVolumeTimestamp) {
        lastVolumeTimestamp = data.timestamp;
        
        if (data.values && Array.isArray(data.values)) {
          volumeSlices = createVolumeSlices(data, 16);
          currentVolumeData = data;
          updateStatus('status3d', `✓ ${data.width}x${data.height}x${data.depth}`);
          return true;
        }
      }
    } catch (error) {
      console.error('Error loading 3D volume:', error);
      updateStatus('status3d', `✗ ${error.message}`);
    }
    return false;
  }

  // Render 3D volume
  function renderVolumeCube(gl, programInfo, volumeSlices) {
    if (!volumeSlices || volumeSlices.length === 0) return;
    
    const fieldOfView = (45 * Math.PI) / 180;
    const aspect = gl.canvas.clientWidth / gl.canvas.clientHeight;
    const projectionMatrix = glMatrix.mat4.create();
    glMatrix.mat4.perspective(projectionMatrix, fieldOfView, aspect, 0.1, 100.0);
    
    gl.disable(gl.CULL_FACE);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    
    const baseMatrix = glMatrix.mat4.create();
    glMatrix.mat4.translate(baseMatrix, baseMatrix, [0.0, 0.0, -6.0]);
    glMatrix.mat4.rotate(baseMatrix, baseMatrix, mouseRotationX, [1, 0, 0]);
    glMatrix.mat4.rotate(baseMatrix, baseMatrix, mouseRotationY, [0, 1, 0]);
    glMatrix.mat4.rotate(baseMatrix, baseMatrix, mouseRotationZ, [0, 0, 1]);
    
    const invRotMatrix = glMatrix.mat4.create();
    glMatrix.mat4.invert(invRotMatrix, baseMatrix);
    
    const viewDir = glMatrix.vec3.fromValues(0, 0, 1);
    glMatrix.vec3.transformMat4(viewDir, viewDir, invRotMatrix);
    
    const slicesWithDepth = volumeSlices.map((slice, index) => {
      const slicePos = glMatrix.vec3.fromValues(0, 0, slice.z);
      const depth = glMatrix.vec3.dot(slicePos, viewDir);
      return { slice, depth, originalZ: slice.z };
    });
    
    slicesWithDepth.sort((a, b) => b.depth - a.depth);
    
    slicesWithDepth.forEach(({ slice, originalZ }) => {
      const modelViewMatrix = glMatrix.mat4.create();
      glMatrix.mat4.copy(modelViewMatrix, baseMatrix);
      glMatrix.mat4.translate(modelViewMatrix, modelViewMatrix, [0, 0, originalZ]);
      glMatrix.mat4.scale(modelViewMatrix, modelViewMatrix, [1.0, 1.0, 0.02]);
      
      const sliceTexture = createTextureFromMatrix(gl, slice.data, slice.width, slice.height);
      renderQuad(gl, programInfo, sliceTexture, projectionMatrix, modelViewMatrix);
    });
    
    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.enable(gl.CULL_FACE);
  }

  // =========================================================================
  // Forward Kinematics 3D Control Points Renderer
  // =========================================================================

  // FK canvas shader: simple colored vertices (no textures)
  const fkVsSource = `
    attribute vec3 aPosition;
    attribute vec3 aColor;
    uniform mat4 uProjectionMatrix;
    uniform mat4 uModelViewMatrix;
    uniform float uPointSize;
    varying lowp vec3 vColor;
    void main(void) {
      gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aPosition, 1.0);
      gl_PointSize = uPointSize;
      vColor = aColor;
    }
  `;

  const fkFsSource = `
    precision mediump float;
    varying lowp vec3 vColor;
    void main(void) {
      gl_FragColor = vec4(vColor, 1.0);
    }
  `;

  const fkShaderProgram = initShaderProgram(glFK, fkVsSource, fkFsSource);
  const fkProgramInfo = {
    program: fkShaderProgram,
    attribLocations: {
      position: glFK.getAttribLocation(fkShaderProgram, "aPosition"),
      color: glFK.getAttribLocation(fkShaderProgram, "aColor"),
    },
    uniformLocations: {
      projectionMatrix: glFK.getUniformLocation(fkShaderProgram, "uProjectionMatrix"),
      modelViewMatrix: glFK.getUniformLocation(fkShaderProgram, "uModelViewMatrix"),
      pointSize: glFK.getUniformLocation(fkShaderProgram, "uPointSize"),

    },
  };

  // Generate a distinct color for each control point index using HSL
  function getPointColor(index, total) {
    const hue = (index / Math.max(total, 1)) * 360;
    const s = 0.9, l = 0.6;
    // HSL to RGB conversion
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const x = c * (1 - Math.abs(((hue / 60) % 2) - 1));
    const m = l - c / 2;
    let r, g, b;
    if (hue < 60) { r = c; g = x; b = 0; }
    else if (hue < 120) { r = x; g = c; b = 0; }
    else if (hue < 180) { r = 0; g = c; b = x; }
    else if (hue < 240) { r = 0; g = x; b = c; }
    else if (hue < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    return [r + m, g + m, b + m];
  }

  // Render forward kinematics control points and connecting lines
  function renderFKControlPoints(gl, programInfo, controlPoints) {
    gl.clearColor(0.05, 0.05, 0.1, 1.0);
    gl.clearDepth(1.0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    if (!controlPoints || controlPoints.length === 0) return;

    gl.useProgram(programInfo.program);

    // Set up projection matrix
    const fieldOfView = (45 * Math.PI) / 180;
    const aspect = gl.canvas.clientWidth / gl.canvas.clientHeight;
    const projectionMatrix = glMatrix.mat4.create();
    glMatrix.mat4.perspective(projectionMatrix, fieldOfView, aspect, 0.1, 100.0);

    // Set up model-view matrix with FK-specific mouse rotation
    const modelViewMatrix = glMatrix.mat4.create();
    glMatrix.mat4.translate(modelViewMatrix, modelViewMatrix, [0.0, 0.0, -15.0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, fkMouseRotationX, [1, 0, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, fkMouseRotationY, [0, 1, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, fkMouseRotationZ, [0, 0, 1]);

    // Center the points around origin by computing centroid
    let cx = 0, cy = 0, cz = 0;
    for (const p of controlPoints) { cx += p.x; cy += p.y; cz += p.z; }
    cx /= controlPoints.length;
    cy /= controlPoints.length;
    cz /= controlPoints.length;

    // Compute scale so points fit in view
    let maxDist = 0.001;
    for (const p of controlPoints) {
      const dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
      const dist = Math.sqrt(dx*dx + dy*dy + dz*dz);
      if (dist > maxDist) maxDist = dist;
    }
    const scale = 4.0 / maxDist; // Fit within ~4 units

    gl.uniformMatrix4fv(programInfo.uniformLocations.projectionMatrix, false, projectionMatrix);
    gl.uniformMatrix4fv(programInfo.uniformLocations.modelViewMatrix, false, modelViewMatrix);

    const numPts = controlPoints.length;

    // Build position and color arrays for points
    const positions = [];
    const colors = [];
    for (let i = 0; i < numPts; i++) {
      const p = controlPoints[i];
      positions.push((p.x - cx) * scale, (p.y - cy) * scale, (p.z - cz) * scale);
      const col = getPointColor(i, numPts);
      colors.push(col[0], col[1], col[2]);
    }

    // Create and bind position buffer
    const posBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(positions), gl.STATIC_DRAW);
    gl.vertexAttribPointer(programInfo.attribLocations.position, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.position);

    // Create and bind color buffer
    const colBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, colBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(colors), gl.STATIC_DRAW);
    gl.vertexAttribPointer(programInfo.attribLocations.color, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.color);

    // Draw lines connecting consecutive control points (white)
    if (numPts >= 2) {
      const linePositions = [];
      const lineColors = [];
      for (let i = 0; i < numPts - 1; i++) {
        const p1 = controlPoints[i];
        const p2 = controlPoints[i + 1];
        linePositions.push(
          (p1.x - cx) * scale, (p1.y - cy) * scale, (p1.z - cz) * scale,
          (p2.x - cx) * scale, (p2.y - cy) * scale, (p2.z - cz) * scale
        );
        lineColors.push(0.5, 0.5, 0.5, 0.5, 0.5, 0.5); // Gray lines
      }

      const linePosBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, linePosBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(linePositions), gl.STATIC_DRAW);
      gl.vertexAttribPointer(programInfo.attribLocations.position, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(programInfo.attribLocations.position);

      const lineColBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, lineColBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(lineColors), gl.STATIC_DRAW);
      gl.vertexAttribPointer(programInfo.attribLocations.color, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(programInfo.attribLocations.color);

      gl.uniform1f(programInfo.uniformLocations.pointSize, 1.0);
      gl.lineWidth(2.0);
      gl.drawArrays(gl.LINES, 0, (numPts - 1) * 2);
    }

    // Rebind point buffers and draw points on top
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
    gl.vertexAttribPointer(programInfo.attribLocations.position, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.position);

    gl.bindBuffer(gl.ARRAY_BUFFER, colBuffer);
    gl.vertexAttribPointer(programInfo.attribLocations.color, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.color);

    gl.uniform1f(programInfo.uniformLocations.pointSize, 12.0);
    gl.drawArrays(gl.POINTS, 0, numPts);

    // Draw small axes at the origin for reference
    const axisLen = 1.5;
    const axisPositions = [
      0, 0, 0,  axisLen, 0, 0,  // X axis
      0, 0, 0,  0, axisLen, 0,  // Y axis
      0, 0, 0,  0, 0, axisLen,  // Z axis
    ];
    const axisColors = [
      1, 0, 0,  1, 0, 0,  // Red = X
      0, 1, 0,  0, 1, 0,  // Green = Y
      0, 0, 1,  0, 0, 1,  // Blue = Z
    ];

    const axisPosBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, axisPosBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(axisPositions), gl.STATIC_DRAW);
    gl.vertexAttribPointer(programInfo.attribLocations.position, 3, gl.FLOAT, false, 0, 0);

    const axisColBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, axisColBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(axisColors), gl.STATIC_DRAW);
    gl.vertexAttribPointer(programInfo.attribLocations.color, 3, gl.FLOAT, false, 0, 0);

    gl.uniform1f(programInfo.uniformLocations.pointSize, 1.0);
    gl.drawArrays(gl.LINES, 0, 6);
  }

  // Mouse controls for FK canvas (independent rotation)
  canvasFK.addEventListener('mousedown', (e) => {
    fkIsMouseDown = true;
    fkLastMouseX = e.clientX;
    fkLastMouseY = e.clientY;
    e.preventDefault();
  });

  canvasFK.addEventListener('mousemove', (e) => {
    if (fkIsMouseDown) {
      const deltaX = e.clientX - fkLastMouseX;
      const deltaY = e.clientY - fkLastMouseY;
      const sensitivity = 0.01;
      fkMouseRotationY += deltaX * sensitivity;
      fkMouseRotationX += deltaY * sensitivity;
      if (e.shiftKey) {
        fkMouseRotationZ += deltaX * sensitivity;
      }
      fkLastMouseX = e.clientX;
      fkLastMouseY = e.clientY;
    }
  });

  canvasFK.addEventListener('mouseup', () => {
    fkIsMouseDown = false;
  });

  canvasFK.addEventListener('mouseleave', () => {
    fkIsMouseDown = false;
  });

  // Mouse controls for 3D volume
  canvas3D.addEventListener('mousedown', (e) => {
    isMouseDown = true;
    lastMouseX = e.clientX;
    lastMouseY = e.clientY;
    e.preventDefault();
  });

  canvas3D.addEventListener('mousemove', (e) => {
    if (isMouseDown) {
      const deltaX = e.clientX - lastMouseX;
      const deltaY = e.clientY - lastMouseY;
      const sensitivity = 0.01;
      mouseRotationY += deltaX * sensitivity;
      mouseRotationX += deltaY * sensitivity;
      if (e.shiftKey) {
        mouseRotationZ += deltaX * sensitivity;
      }
      lastMouseX = e.clientX;
      lastMouseY = e.clientY;
    }
  });

  canvas3D.addEventListener('mouseup', () => {
    isMouseDown = false;
  });

  canvas3D.addEventListener('mouseleave', () => {
    isMouseDown = false;
  });

  // Click handler for 2D canvas
  canvas2D.addEventListener('click', (e) => {
    const rect = canvas2D.getBoundingClientRect();
    const mouseX = e.clientX - rect.left;
    const mouseY = e.clientY - rect.top;
    handleCanvas2DClick(mouseX, mouseY);
  });

  console.log('Loading initial data...');
  updateStatus('status', 'Loading data from server...');
  await updateTextureFromServer();
  await updateVolumeFromServer();
  
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);

  let then = 0;

  function render(now) {
    now *= 0.001;
    deltaTime = now - then;
    then = now;

    updateTextureFromServer().catch(() => {});
    updateVolumeFromServer().catch(() => {});
    updateTipPoseFromServer().catch(() => {});
    updateForwardKinematicsFromServer().catch(() => {});

    gl.clearColor(0.0, 0.0, 0.0, 1.0);
    gl.clearDepth(1.0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    renderVolumeCube(gl, programInfo, volumeSlices);

    // Render FK control points on the separate canvas
    renderFKControlPoints(glFK, fkProgramInfo, fkControlPoints);

    requestAnimationFrame(render);
  }
  requestAnimationFrame(render);
}

function initShaderProgram(gl, vsSource, fsSource) {
  const vertexShader = loadShader(gl, gl.VERTEX_SHADER, vsSource);
  const fragmentShader = loadShader(gl, gl.FRAGMENT_SHADER, fsSource);

  const shaderProgram = gl.createProgram();
  gl.attachShader(shaderProgram, vertexShader);
  gl.attachShader(shaderProgram, fragmentShader);
  gl.linkProgram(shaderProgram);

  if (!gl.getProgramParameter(shaderProgram, gl.LINK_STATUS)) {
    alert(`Unable to initialize the shader program: ${gl.getProgramInfoLog(shaderProgram)}`);
    return null;
  }

  return shaderProgram;
}

function loadShader(gl, type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);

  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    alert(`An error occurred compiling the shaders: ${gl.getShaderInfoLog(shader)}`);
    gl.deleteShader(shader);
    return null;
  }

  return shader;
}

function createTextureFromMatrix(gl, matrix, width, height) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);

  let minValue = Infinity;
  let maxValue = -Infinity;
  
  for (let i = 0; i < matrix.length; i++) {
    const val = matrix[i];
    if (val < minValue) minValue = val;
    if (val > maxValue) maxValue = val;
  }
  
  const range = maxValue - minValue > 0.001 ? maxValue - minValue : 1.0;
  
  const rgbaData = new Uint8Array(width * height * 4);
  
  for (let i = 0; i < matrix.length; i++) {
    const normalized = (matrix[i] - minValue) / range;
    const value = Math.min(1.0, Math.max(0.0, normalized));
    const byteValue = Math.round(value * 255);
    rgbaData[i * 4] = byteValue;
    rgbaData[i * 4 + 1] = byteValue;
    rgbaData[i * 4 + 2] = byteValue;
    rgbaData[i * 4 + 3] = byteValue;
  }

  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, rgbaData);
  
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

  return texture;
}