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
  const gl = canvas3D.getContext("webgl");
  const gl2d = canvas2D.getContext("2d");

  if (gl === null) {
    alert("Unable to initialize WebGL. Your browser or machine may not support it.");
    return;
  }

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

    gl.clearColor(0.0, 0.0, 0.0, 1.0);
    gl.clearDepth(1.0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    renderVolumeCube(gl, programInfo, volumeSlices);

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