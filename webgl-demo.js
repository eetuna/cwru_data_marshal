// WebGL rendering engine - renders 3D volume with separate 2D image canvas
import { Image2DRenderer } from "./render-2d-image.js";

let deltaTime = 0;
let mouseRotationX = 0.0;
let mouseRotationY = 0.0;
let mouseRotationZ = 0.0;
let volumeZoom = -6.0;
let sliceHistoryZoom = -90.0;
let isMouseDown = false;
let lastMouseX = 0;
let lastMouseY = 0;
let sliceHistoryRotationX = -0.3;
let sliceHistoryRotationY = 0.4;
let sliceHistoryRotationZ = 0.0;
let isSliceHistoryMouseDown = false;
let lastSliceHistoryMouseX = 0;
let lastSliceHistoryMouseY = 0;
function initCurrentSliders() {
  for (let i = 1; i <= 6; i++) {
    const slider = document.getElementById(`sliderI${i}`);
    const valSpan = document.getElementById(`valI${i}`);
    if (!slider || !valSpan) continue;
    slider.value = '0';
    valSpan.textContent = '0.00';
    slider.addEventListener('input', () => {
      valSpan.textContent = parseFloat(slider.value).toFixed(2);
    });
  }
}

function initRotationSliders() {
  const sliderIds = [
    { sliderId: 'sliderRotX', valueId: 'valRotX' },
    { sliderId: 'sliderRotY', valueId: 'valRotY' },
    { sliderId: 'sliderRotZ', valueId: 'valRotZ' }
  ];

  for (const { sliderId, valueId } of sliderIds) {
    const slider = document.getElementById(sliderId);
    const valSpan = document.getElementById(valueId);
    if (!slider || !valSpan) continue;
    slider.value = '0';
    valSpan.dataset.lastApplied = '0';
    valSpan.textContent = '0 (last 0)';
    slider.addEventListener('input', () => {
      const pending = parseInt(slider.value, 10);
      const lastApplied = valSpan.dataset.lastApplied || '0';
      valSpan.textContent = `${Number.isFinite(pending) ? pending : 0} (last ${lastApplied})`;
    });
  }
}
main();

async function main() {
  const canvas3DEl = document.querySelector("#glcanvas");
  const canvas3D = canvas3DEl || document.createElement('canvas');
  if (!canvas3DEl) {
    canvas3D.width = 1280;
    canvas3D.height = 960;
  }
  const canvas2D = document.querySelector("#canvas2d");
  const canvasSlices3D = document.querySelector("#canvasSlices3d");
  const savedTransformCanvases = [
    document.querySelector("#canvasSavedTransform1"),
    document.querySelector("#canvasSavedTransform2"),
    document.querySelector("#canvasSavedTransform3")
  ];
  const canvasFKEl = document.querySelector("#canvasFK");
  const canvasFK = canvasFKEl || document.createElement('canvas');
  if (!canvasFKEl) {
    canvasFK.width = 960;
    canvasFK.height = 720;
  }
  const gl = canvas3D.getContext("webgl");
  const gl2d = canvas2D.getContext("2d");
  const glSlices = canvasSlices3D ? canvasSlices3D.getContext("webgl") : null;
  const savedTransformCtxs = savedTransformCanvases.map((canvas) => canvas ? canvas.getContext("2d") : null);
  const glFK = canvasFK.getContext("webgl");

  if (gl === null) {
    alert("Unable to initialize WebGL. Your browser or machine may not support it.");
    return;
  }

  if (glFK === null) {
    alert("Unable to initialize WebGL for FK canvas.");
    return;
  }

  if (canvasSlices3D && glSlices === null) {
    updateStatus('status3dSlices', '✗ WebGL init failed');
  }

  // FK canvas mouse rotation state (independent from 3D volume)
  let fkMouseRotationX = -0.5;
  let fkMouseRotationY = 0.3;
  let fkMouseRotationZ = 0.0;
  let fkIsMouseDown = false;
  let fkLastMouseX = 0;
  let fkLastMouseY = 0;
  let fkZoom = -25.0;  // Camera distance (scroll to zoom)

  // Forward kinematics control points: array of {x, y, z}
  let fkControlPoints = [];
  let fkFixedCentroid = null;  // Set once from first frame
  let fkFixedScale = null;     // Set once from first frame

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

  const sliceShaderProgram = glSlices ? initShaderProgram(glSlices, vsSource, fsSource) : null;
  const sliceProgramInfo = glSlices && sliceShaderProgram ? {
    program: sliceShaderProgram,
    attribLocations: {
      vertexPosition: glSlices.getAttribLocation(sliceShaderProgram, "aVertexPosition"),
      textureCoord: glSlices.getAttribLocation(sliceShaderProgram, "aTextureCoord"),
    },
    uniformLocations: {
      projectionMatrix: glSlices.getUniformLocation(sliceShaderProgram, "uProjectionMatrix"),
      modelViewMatrix: glSlices.getUniformLocation(sliceShaderProgram, "uModelViewMatrix"),
      uSampler: glSlices.getUniformLocation(sliceShaderProgram, "uSampler"),
    },
  } : null;

  const readServerUrl = "http://localhost:3000";    // Main server (handles routing and fallbacks)
  const writeServerUrl = "http://localhost:3001";   // Backend write server (for writing data)
  const clientId = "client-webgl";
  
  let volumeSlices = null;
  let currentVolumeData = null;
  let currentImageData = null;
  let currentImageHistory = [];
  let currentSnapshotSlices = [];
  let currentDisplayedSlices = [];
  let sliceDisplayCursor = 0;
  let lastSliceCount = 0;
  let lastTimestamp = -1;
  let lastVolumeTimestamp = -1;
  let lastTipTimestamp = -1;
  let lastFKTimestamp = -1;
  let lastForceTimestamp = -1;
  let currentForceSensingData = null;
  let savedTransformDataBySlot = [null, null, null];

  // Create 2D image renderer
  const image2DRenderer = new Image2DRenderer(gl);
initCurrentSliders();
initRotationSliders();

  function renderSavedTransformCanvas(ctx, transformData, slotIndex) {
    if (!ctx) return;

    const { canvas } = ctx;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#050505';
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    ctx.strokeStyle = '#333';
    ctx.strokeRect(0.5, 0.5, canvas.width - 1, canvas.height - 1);

    ctx.fillStyle = '#ff66aa';
    ctx.font = '26px monospace';
    ctx.textBaseline = 'top';
    ctx.fillText(`saved_transform_${slotIndex + 1}.json`, 18, 16);

    if (!transformData) {
      ctx.fillStyle = '#aaa';
      ctx.font = '22px monospace';
      ctx.fillText('No saved transform yet.', 18, 64);
      return;
    }

    const pos = Array.isArray(transformData.position) ? transformData.position : null;
    const ori = transformData.orientation || null;
    const readDir = ori && Number.isFinite(ori.m00) ? [ori.m00, ori.m10, ori.m20] : null;
    const phaseDir = ori && Number.isFinite(ori.m01) ? [ori.m01, ori.m11, ori.m21] : null;
    const sliceDir = ori && Number.isFinite(ori.m02) ? [ori.m02, ori.m12, ori.m22] : null;
    const px = Array.isArray(transformData.pixelSize) ? transformData.pixelSize : null;

    const imageWidth = Number.isFinite(transformData.image_width) ? transformData.image_width : null;
    const imageHeight = Number.isFinite(transformData.image_height) ? transformData.image_height : null;
    const imageValues = Array.isArray(transformData.image_values) ? transformData.image_values : null;

    const drawImageFromValues = () => {
      if (!(imageWidth && imageHeight && imageValues && imageValues.length)) {
        ctx.fillStyle = '#222';
        ctx.fillRect(18, 64, canvas.width - 36, 208);
        ctx.fillStyle = '#888';
        ctx.font = '20px monospace';
        ctx.fillText('No saved image snapshot.', 30, 124);
        return;
      }

      let minValue = Infinity;
      let maxValue = -Infinity;
      for (let i = 0; i < imageValues.length; i++) {
        if (imageValues[i] < minValue) minValue = imageValues[i];
        if (imageValues[i] > maxValue) maxValue = imageValues[i];
      }
      const range = maxValue - minValue > 0.001 ? maxValue - minValue : 1.0;

      const rgbaData = new Uint8ClampedArray(imageWidth * imageHeight * 4);
      for (let i = 0; i < imageValues.length; i++) {
        const normalized = (imageValues[i] - minValue) / range;
        const value = Math.min(1.0, Math.max(0.0, normalized));
        const byteValue = Math.round(value * 255);
        rgbaData[i * 4] = byteValue;
        rgbaData[i * 4 + 1] = byteValue;
        rgbaData[i * 4 + 2] = byteValue;
        rgbaData[i * 4 + 3] = 255;
      }

      const tmpCanvas = document.createElement('canvas');
      tmpCanvas.width = imageWidth;
      tmpCanvas.height = imageHeight;
      const tmpCtx = tmpCanvas.getContext('2d');
      if (!tmpCtx) return;
      tmpCtx.putImageData(new ImageData(rgbaData, imageWidth, imageHeight), 0, 0);

      const imageAreaX = 18;
      const imageAreaY = 64;
      const imageAreaWidth = canvas.width - 36;
      const imageAreaHeight = 208;
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(tmpCanvas, 0, 0, imageWidth, imageHeight, imageAreaX, imageAreaY, imageAreaWidth, imageAreaHeight);
      ctx.strokeStyle = '#666';
      ctx.strokeRect(imageAreaX + 0.5, imageAreaY + 0.5, imageAreaWidth - 1, imageAreaHeight - 1);
    };

    drawImageFromValues();

    const lines = [
      `frame_index: ${transformData.frame_index ?? '-'}`,
      `saved_at: ${transformData.saved_at ?? '-'}`,
      pos ? `position: [${pos.map(v => v.toFixed(3)).join(', ')}]` : 'position: missing',
      readDir ? `read_dir: [${readDir.map(v => v.toFixed(3)).join(', ')}]` : 'read_dir: missing',
      phaseDir ? `phase_dir: [${phaseDir.map(v => v.toFixed(3)).join(', ')}]` : 'phase_dir: missing',
      sliceDir ? `slice_dir: [${sliceDir.map(v => v.toFixed(3)).join(', ')}]` : 'slice_dir: missing',
      px ? `pixelSize: [${px.map(v => v.toFixed(3)).join(', ')}]` : 'pixelSize: missing'
    ];

    ctx.font = '20px monospace';
    lines.forEach((line, idx) => {
      const y = 72 + idx * 28;
      ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
      ctx.fillRect(26, y - 2, canvas.width - 52, 24);
      ctx.fillStyle = '#f5f5f5';
      ctx.fillText(line, 32, y);
    });
  }

  savedTransformCtxs.forEach((ctx, idx) => {
    renderSavedTransformCanvas(ctx, savedTransformDataBySlot[idx], idx);
  });
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

    // Free per-call buffers: renderQuad runs several times per animation
    // frame, and undeleted buffer objects accumulate just like the texture
    // leak fixed alongside this (same context-lost failure class).
    gl.deleteBuffer(posBuffer);
    gl.deleteBuffer(texBuffer);
    gl.deleteBuffer(idxBuffer);
  }

  // Render the latest 3 slices side-by-side on the 2D canvas.
  function render2DImage(ctx, imagePanels) {
    if (!Array.isArray(imagePanels) || imagePanels.length === 0) return;

    const maxPanels = 3;
    const panels = imagePanels.slice(0, maxPanels);
    const targetWidth = Math.max(...panels.map(img => img?.width || 1));
    const targetHeight = Math.max(...panels.map(img => img?.height || 1));
    const gap = 8;

    ctx.canvas.width = targetWidth * maxPanels + gap * (maxPanels - 1);
    ctx.canvas.height = targetHeight;
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
    ctx.font = '12px sans-serif';
    ctx.textBaseline = 'top';

    const fitTextToWidth = (text, maxWidthPx) => {
      if (ctx.measureText(text).width <= maxWidthPx) return text;
      const suffix = '...';
      let end = text.length;
      while (end > 0) {
        const candidate = text.slice(0, end) + suffix;
        if (ctx.measureText(candidate).width <= maxWidthPx) {
          return candidate;
        }
        end -= 1;
      }
      return suffix;
    };

    for (let panelIdx = 0; panelIdx < maxPanels; panelIdx++) {
      const offsetX = panelIdx * (targetWidth + gap);
      const imageData = panels[panelIdx];

      if (!imageData || !Array.isArray(imageData.values)) {
        ctx.fillStyle = '#111';
        ctx.fillRect(offsetX, 0, targetWidth, targetHeight);
        ctx.strokeStyle = '#333';
        ctx.strokeRect(offsetX + 0.5, 0.5, targetWidth - 1, targetHeight - 1);
        ctx.fillStyle = '#888';
        ctx.fillText('No slice', offsetX + 8, 8);
        continue;
      }

      const { width, height, values } = imageData;
      let minValue = Infinity;
      let maxValue = -Infinity;
      for (let i = 0; i < values.length; i++) {
        if (values[i] < minValue) minValue = values[i];
        if (values[i] > maxValue) maxValue = values[i];
      }
      const range = maxValue - minValue > 0.001 ? maxValue - minValue : 1.0;

      const slicePixels = new Uint8ClampedArray(width * height * 4);
      for (let i = 0; i < values.length; i++) {
        const normalized = (values[i] - minValue) / range;
        const value = Math.min(1.0, Math.max(0.0, normalized));
        const byteValue = Math.round(value * 255);
        slicePixels[i * 4] = byteValue;
        slicePixels[i * 4 + 1] = byteValue;
        slicePixels[i * 4 + 2] = byteValue;
        slicePixels[i * 4 + 3] = 255;
      }

      const tmpCanvas = document.createElement('canvas');
      tmpCanvas.width = width;
      tmpCanvas.height = height;
      const tmpCtx = tmpCanvas.getContext('2d');
      if (!tmpCtx) {
        continue;
      }
      tmpCtx.putImageData(new ImageData(slicePixels, width, height), 0, 0);

      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(tmpCanvas, 0, 0, width, height, offsetX, 0, targetWidth, targetHeight);

      ctx.strokeStyle = '#333';
      ctx.strokeRect(offsetX + 0.5, 0.5, targetWidth - 1, targetHeight - 1);

      const frame = Number.isFinite(imageData.frame_index) ? imageData.frame_index : '-';
      const sliceLabel = Number.isFinite(imageData.slice) ? imageData.slice : (Number.isFinite(imageData.slice_index) ? imageData.slice_index : '-');
      const annotationLines = [`#${panelIdx + 1} f ${frame} s ${sliceLabel}`];
      if (Array.isArray(imageData.position) && imageData.position.length === 3) {
        const p = imageData.position;
        annotationLines.push(`pos [${p[0].toFixed(1)}, ${p[1].toFixed(1)}, ${p[2].toFixed(1)}]`);
      }

      const padding = 8;
      const fontSize = Math.max(10, Math.min(14, Math.floor(Math.min(targetWidth / 22, targetHeight / 10))));
      const lineHeight = fontSize + 4;
      const maxLines = Math.max(1, Math.floor((targetHeight - padding * 2) / lineHeight));
      const drawCount = Math.min(annotationLines.length, maxLines);
      const maxTextWidth = Math.max(24, targetWidth - padding * 2 - 4);

      ctx.save();
      ctx.beginPath();
      ctx.rect(offsetX + 1, 1, targetWidth - 2, targetHeight - 2);
      ctx.clip();
      ctx.font = `${fontSize}px sans-serif`;

      for (let lineIdx = 0; lineIdx < drawCount; lineIdx++) {
        const y = padding + lineIdx * lineHeight;
        const rawText = annotationLines[lineIdx];
        const lineText = fitTextToWidth(rawText, maxTextWidth);
        const measured = Math.min(maxTextWidth, ctx.measureText(lineText).width);

        ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
        ctx.fillRect(offsetX + padding - 2, y - 1, measured + 8, lineHeight - 1);
        ctx.fillStyle = lineIdx === 0 ? '#0ff' : '#ffd43b';
        ctx.fillText(lineText, offsetX + padding, y);
      }
      ctx.restore();
    }
  }

  // Get pixel from 2D image click
  function getPixelFromImage2DClick(screenX, screenY, canvas, imagePanels) {
    if (!Array.isArray(imagePanels) || imagePanels.length === 0) return null;

    const maxPanels = 3;
    const gap = 8;
    const panels = imagePanels.slice(0, maxPanels);
    const targetWidth = Math.max(...panels.map(img => img?.width || 1));
    const targetHeight = Math.max(...panels.map(img => img?.height || 1));
    const panelSpan = targetWidth + gap;

    const canvasX = (screenX / canvas.clientWidth) * canvas.width;
    const canvasY = (screenY / canvas.clientHeight) * canvas.height;

    if (canvasY < 0 || canvasY > targetHeight) return null;

    const panelIndex = Math.max(0, Math.min(maxPanels - 1, Math.floor(canvasX / panelSpan)));
    const panelLocalX = canvasX - panelIndex * panelSpan;
    if (panelLocalX < 0 || panelLocalX > targetWidth) return null;

    const imageData = panels[panelIndex];
    if (!imageData || !Array.isArray(imageData.values)) return null;

    const { width, height, values } = imageData;
    const pixelX = Math.floor((panelLocalX / targetWidth) * width);
    const pixelY = Math.floor((canvasY / targetHeight) * height);

    const x = Math.max(0, Math.min(width - 1, pixelX));
    const y = Math.max(0, Math.min(height - 1, pixelY));
    const index = y * width + x;
    const pixelValue = values[index];

    return {
      pixelX: x,
      pixelY: y,
      value: pixelValue,
      panelIndex,
      frameIndex: imageData.frame_index
    };
  }

  // Handle 2D canvas clicks
  function handleCanvas2DClick(mouseX, mouseY) {
    console.log(`[CLICK] 2D canvas click at (${mouseX.toFixed(0)}, ${mouseY.toFixed(0)})`);

    const pixelHit = getPixelFromImage2DClick(mouseX, mouseY, canvas2D, currentDisplayedSlices);
    if (pixelHit) {
      updateStatus('selectedPoint', `[${pixelHit.pixelX}, ${pixelHit.pixelY}]`);
      updateStatus('pointValue', `${(pixelHit.value * 255).toFixed(0)} / 255`);
      console.log(`✓ Selected pixel [${pixelHit.pixelX}, ${pixelHit.pixelY}] on panel ${pixelHit.panelIndex + 1}, value=${pixelHit.value.toFixed(4)}, frame=${pixelHit.frameIndex}`);
      
      // Send pixel coordinates to C++ server
      postPixelCoordinatesToServer(pixelHit.pixelX, pixelHit.pixelY);
    }
  }

  // POST rendered 2D image data to C++ server (write_to3 = fileKey 2)
  async function postRendered2DImageToServer(imageData) {
    try {
      const receivedInWebglDemoAt = imageData.received_in_webgl_demo_at || Date.now();
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        width: imageData.width,
        height: imageData.height,
        frame_index: imageData.frame_index,
        sent_from_serverjs: imageData.sent_from_serverjs || null,
        metadata_duration_ms: imageData.metadata_duration_ms ?? null,
        hdf5_read_duration_ms: imageData.hdf5_read_duration_ms ?? null,
        received_in_webgl_demo_at: receivedInWebglDemoAt,
        serverjs_to_webgl_demo_ms: typeof imageData.sent_from_serverjs === 'number'
          ? receivedInWebglDemoAt - imageData.sent_from_serverjs
          : null,
        values: [1]
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/2`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      console.log(`✓ Rendered 2D image posted to server (${imageData.width}x${imageData.height})`);
    } catch (error) {
      console.error('Error posting rendered 2D image:', error);
    }
  }

  // POST will-render 2D image marker to C++ server (write_to4 = fileKey 3)
  async function postWillRender2DImageToServer(imageData) {
    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        width: imageData.width,
        height: imageData.height,
        frame_index: imageData.frame_index,
        values: [1]
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/3`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      console.log(`✓ Will-render 2D image posted to server (${imageData.width}x${imageData.height})`);
    } catch (error) {
      console.error('Error posting will-render 2D image:', error);
    }
  }

  // POST marker right before updateTextureFromServer fetch starts (write_to5 = fileKey 4)
  async function postWillUpdateTextureFromServerToServer(imageData) {
    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        width: imageData?.width || 0,
        height: imageData?.height || 0,
        frame_index: imageData?.frame_index,
        values: [1]
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/4`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    } catch (error) {
      console.error('Error posting will_update_texture_from_server:', error);
    }
  }

  // POST marker right after updateTextureFromServer fetch finishes (write_to6 = fileKey 5)
  async function postUpdatedTextureFromServerToServer(imageData) {
    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        width: imageData?.width || 0,
        height: imageData?.height || 0,
        frame_index: imageData?.frame_index,
        values: [1]
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/5`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    } catch (error) {
      console.error('Error posting updated_texture_from_server:', error);
    }
  }

  // POST marker right before updateForceSensingFromServer fetch starts (write_to7 = fileKey 6)
  async function postWillUpdateForceSensingFromServerToServer(forceData) {
    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        source_timestamp: forceData?.sent_at || forceData?.received_at || forceData?.timestamp || 0,
        values: [1]
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/6`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    } catch (error) {
      console.error('Error posting will_update_force_sensing_from_server:', error);
    }
  }

  // POST marker right after updateForceSensingFromServer fetch finishes (write_to8 = fileKey 7)
  async function postUpdatedForceSensingFromServerToServer(forceData) {
    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        source_timestamp: forceData?.sent_at || forceData?.received_at || forceData?.timestamp || 0,
        values: [1]
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/7`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    } catch (error) {
      console.error('Error posting updated_force_sensing_from_server:', error);
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
      
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/1`, {
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

  // POST relative slice delta command to MRI marshal via backend write proxy.
  // Payload contract:
  // {"translation_mm": [dx,dy,dz], "rotation_rad": [rx,ry,rz]}
  async function postSliceDeltaToServer(translationMm, rotationRad) {
    if (!Array.isArray(translationMm) || translationMm.length !== 3 || !translationMm.every(Number.isFinite)) {
      return;
    }
    if (!Array.isArray(rotationRad) || rotationRad.length !== 3 || !rotationRad.every(Number.isFinite)) {
      return;
    }

    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        translation_mm: translationMm,
        rotation_rad: rotationRad
      };

      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/8`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(payload)
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      updateStatus('debug', `Sent slice_delta t=[${translationMm.map(v => v.toFixed(2)).join(', ')}] r=[${rotationRad.map(v => v.toFixed(3)).join(', ')}]`);
    } catch (error) {
      console.error('Error posting slice_delta:', error);
      updateStatus('debug', `✗ Failed to post slice_delta: ${error.message}`);
    }
  }

  // POST slice translation command (+1 or -1) as slice_delta translation.
  async function postSliceTranslationToServer(direction) {
    if (direction !== 1 && direction !== -1) {
      return;
    }

    // Preserve legacy button semantics: ±1 mm along slice axis.
    await postSliceDeltaToServer([0, 0, direction], [0, 0, 0]);
  }

  // POST slice thickness command [1..15] to file_slice_thickness endpoint
  async function postSliceThicknessToServer(thickness) {
    if (!Number.isFinite(thickness) || thickness < 1 || thickness > 15) {
      return;
    }

    try {
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        values: [thickness]
      };

      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/13`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(payload)
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      updateStatus('debug', `Sent slice thickness ${thickness}`);
    } catch (error) {
      console.error('Error posting slice thickness:', error);
      updateStatus('debug', `✗ Failed to post slice thickness: ${error.message}`);
    }
  }

  const savedTransformWriteFileKeys = [14, 16, 18];
  const sliceTargetWriteFileKeys = [15, 17, 19];

  async function postSavedTransformToServer(slotIndex, imageData) {
    const pos = Array.isArray(imageData?.position) && imageData.position.length === 3 ? imageData.position : null;
    const ori = imageData?.orientation || null;

    if (!(pos && ori)) {
      updateStatus(`savedTransformStatus${slotIndex + 1}`, 'Missing image header pose');
      updateStatus('debug', 'Cannot save transform: latest image header pose missing');
      return;
    }

    const payload = {
      client_id: clientId,
      sent_at: Date.now(),
      // Robot marshal requires a values array on every write payload.
      values: [
        ...(Array.isArray(imageData.position) ? imageData.position : []),
        ...(Number.isFinite(ori?.m00) ? [ori.m00, ori.m01, ori.m02, ori.m10, ori.m11, ori.m12, ori.m20, ori.m21, ori.m22] : [])
      ],
      saved_at: new Date().toISOString(),
      image_timestamp: imageData.timestamp ?? null,
      frame_index: imageData.frame_index ?? null,
      position: imageData.position,
      orientation: imageData.orientation,
      pixelSize: imageData.pixelSize ?? null,
      image_width: imageData.width ?? null,
      image_height: imageData.height ?? null,
      image_values: Array.isArray(imageData.values) ? [...imageData.values] : null
    };

    try {
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/${savedTransformWriteFileKeys[slotIndex]}`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(payload)
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      savedTransformDataBySlot[slotIndex] = payload;
      renderSavedTransformCanvas(savedTransformCtxs[slotIndex], savedTransformDataBySlot[slotIndex], slotIndex);
      const sendAbsolutePositionBtn = document.getElementById(`sendAbsolutePositionBtn${slotIndex + 1}`);
      if (sendAbsolutePositionBtn) {
        sendAbsolutePositionBtn.disabled = false;
      }
      updateStatus(`savedTransformStatus${slotIndex + 1}`, `Saved frame ${payload.frame_index ?? '-'} to saved_transform_${slotIndex + 1}.json`);
      updateStatus('debug', `Saved transform ${slotIndex + 1} pos=(${pos.map(v => v.toFixed(2)).join(', ')})`);
    } catch (error) {
      console.error('Error posting saved transform:', error);
      updateStatus(`savedTransformStatus${slotIndex + 1}`, `Save failed: ${error.message}`);
      updateStatus('debug', `✗ Failed to save transform ${slotIndex + 1}: ${error.message}`);
    }
  }

  async function postSavedTransformToSliceTarget(slotIndex, transformData) {
    const pos = Array.isArray(transformData?.position) && transformData.position.length === 3 ? transformData.position : null;
    const ori = transformData?.orientation || null;

    if (!(pos && ori)) {
      updateStatus(`savedTransformStatus${slotIndex + 1}`, 'No saved transform available');
      updateStatus('debug', 'Cannot send absolute position: saved transform missing');
      return;
    }

    const readDir = [ori.m00, ori.m10, ori.m20];
    const phaseDir = [ori.m01, ori.m11, ori.m21];
    const sliceDir = [ori.m02, ori.m12, ori.m22];

    const payload = {
      position: pos,
      read_dir: readDir,
      phase_dir: phaseDir,
      slice_dir: sliceDir
    };

    try {
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/${sliceTargetWriteFileKeys[slotIndex]}`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(payload)
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      const result = await response.json();
      updateStatus(`savedTransformStatus${slotIndex + 1}`, `Sent absolute position to slice_target (${result.delivered ? 'delivered' : 'cached'})`);
      updateStatus('debug', `Sent absolute slice target ${slotIndex + 1} pos=(${pos.map(v => v.toFixed(2)).join(', ')})`);
    } catch (error) {
      console.error('Error posting absolute position:', error);
      updateStatus(`savedTransformStatus${slotIndex + 1}`, `Send failed: ${error.message}`);
      updateStatus('debug', `✗ Failed to send absolute position ${slotIndex + 1}: ${error.message}`);
    }
  }

  async function postRotationDeltaToPoseTransform(axis, degrees) {
    if (!Number.isFinite(degrees) || degrees < -180 || degrees > 180 || degrees === 0) {
      return;
    }

    const rad = (degrees * Math.PI) / 180.0;
    const rotationRad = {
      x: [rad, 0, 0],
      y: [0, rad, 0],
      z: [0, 0, rad],
    };

    await postSliceDeltaToServer([0, 0, 0], rotationRad[axis] || [0, 0, 0]);
    updateStatus('debug', `Applied ${axis.toUpperCase()} rotation delta ${degrees} deg`);
  }

  // Fetch 2D image from server
  function formatHeaderPoseForDebug(data) {
    const pos = Array.isArray(data?.position) && data.position.length === 3 ? data.position : null;
    const ori = data?.orientation || null;
    const px = Array.isArray(data?.pixelSize) && data.pixelSize.length >= 2 ? data.pixelSize : null;
    const sliceIdx = Number.isFinite(data?.slice)
      ? data.slice
      : (Number.isFinite(data?.slice_index) ? data.slice_index : null);

    const readDir = (ori && Number.isFinite(ori.m00) && Number.isFinite(ori.m10) && Number.isFinite(ori.m20))
      ? [ori.m00, ori.m10, ori.m20] : null;
    const phaseDir = (ori && Number.isFinite(ori.m01) && Number.isFinite(ori.m11) && Number.isFinite(ori.m21))
      ? [ori.m01, ori.m11, ori.m21] : null;
    const sliceDir = (ori && Number.isFinite(ori.m02) && Number.isFinite(ori.m12) && Number.isFinite(ori.m22))
      ? [ori.m02, ori.m12, ori.m22] : null;

    if (!(pos && readDir && phaseDir && sliceDir)) {
      return `header pose missing (frame ${data?.frame_index ?? '-'})`;
    }

    const posStr = `pos=(${pos[0].toFixed(2)}, ${pos[1].toFixed(2)}, ${pos[2].toFixed(2)})`;
    const readStr = `read=(${readDir[0].toFixed(3)}, ${readDir[1].toFixed(3)}, ${readDir[2].toFixed(3)})`;
    const phaseStr = `phase=(${phaseDir[0].toFixed(3)}, ${phaseDir[1].toFixed(3)}, ${phaseDir[2].toFixed(3)})`;
    const sliceStr = `slice=(${sliceDir[0].toFixed(3)}, ${sliceDir[1].toFixed(3)}, ${sliceDir[2].toFixed(3)})`;
    const sliceIdxStr = sliceIdx !== null ? ` slice_idx=${sliceIdx}` : '';
    const pxStr = px ? ` px=(${px[0].toFixed(3)}, ${px[1].toFixed(3)})` : '';
    return `frame ${data?.frame_index ?? '-'}${sliceIdxStr} ${posStr} ${readStr} ${phaseStr} ${sliceStr}${pxStr}`;
  }

  function formatDisplayedSliceIndicesForDebug(slices) {
    if (!Array.isArray(slices) || slices.length === 0) return 'displayed=[]';
    const labels = slices.map((s) => {
      if (!s) return '-';
      if (Number.isFinite(s.slice)) return s.slice;
      if (Number.isFinite(s.slice_index)) return s.slice_index;
      return '-';
    });
    return `displayed=[${labels.join(',')}]`;
  }

  function isRenderableSliceRecord(slice) {
    return !!(
      slice &&
      Array.isArray(slice.values) &&
      Number.isFinite(slice.width) &&
      Number.isFinite(slice.height) &&
      slice.width > 0 &&
      slice.height > 0 &&
      slice.geometry_valid !== false
    );
  }

  function getSequentialDisplayPanels(slices, maxPanels = 3) {
    if (!Array.isArray(slices) || slices.length === 0) return [];
    if (slices.length <= maxPanels) {
      sliceDisplayCursor = 0;
      return slices.slice();
    }

    if (sliceDisplayCursor >= slices.length) {
      sliceDisplayCursor = 0;
    }

    const panels = [];
    for (let i = 0; i < maxPanels; i++) {
      const idx = (sliceDisplayCursor + i) % slices.length;
      panels.push(slices[idx]);
    }

    // Keep panel cycling behavior analogous to existing frame history shifting.
    sliceDisplayCursor = (sliceDisplayCursor + 1) % slices.length;
    return panels;
  }

  async function updateTextureFromServer() {
    //postWillRender2DImageToServer(data);
    try {
     //postWillRender2DImageToServer(data);
      const response = await fetch(`${readServerUrl}/api/read/${clientId}/0`);
      
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      //postWillRender2DImageToServer(data);
      const data = await response.json();
      data.received_in_webgl_demo_at = Date.now();
      //postWillRender2DImageToServer(data);
      
      if (data.timestamp && data.timestamp !== lastTimestamp) {
        lastTimestamp = data.timestamp;
        
        // New payload path: one snapshot can carry multiple per-slice records.
        const payloadSlices = Array.isArray(data.slices) ? data.slices : [];
        const renderableSlices = payloadSlices
          .filter(isRenderableSliceRecord)
          .map((slice, idx) => ({
            ...slice,
            frame_index: data.frame_index,
            timestamp: data.timestamp,
            slice_index: Number.isFinite(slice.slice_index) ? slice.slice_index : idx,
          }));

        // Backward-compatible single-slice path.
        const fallbackSingle = (data.values && Array.isArray(data.values)) ? [{
          ...data,
          slice_index: 0,
          geometry_valid: data.geometry_valid !== false,
        }] : [];

        const usingCompatibilityFallback = renderableSlices.length === 0 && fallbackSingle.length > 0;
        const activeSlices = renderableSlices.length ? renderableSlices : fallbackSingle;

        if (activeSlices.length > 0) {
          currentSnapshotSlices = activeSlices;
          if (lastSliceCount !== currentSnapshotSlices.length) {
            sliceDisplayCursor = 0;
            lastSliceCount = currentSnapshotSlices.length;
          }

          currentDisplayedSlices = getSequentialDisplayPanels(currentSnapshotSlices, 3);
          currentImageHistory = currentDisplayedSlices.slice();

          const preferredSelected = Number.isFinite(data.selected_slice_index)
            ? currentSnapshotSlices.find((s) => s.slice_index === data.selected_slice_index || s.source_index === data.selected_slice_index)
            : null;
          currentImageData = preferredSelected || currentDisplayedSlices[0] || currentSnapshotSlices[0];

          postWillRender2DImageToServer(currentImageData);
          render2DImage(gl2d, currentDisplayedSlices);
          postRendered2DImageToServer(currentImageData);
          
          // Extract metadata and update renderer
          const metadata = {
            position: currentImageData.position,
            orientation: currentImageData.orientation,
            pixelSize: currentImageData.pixelSize
          };
          image2DRenderer.updateImage(currentImageData.values, currentImageData.width, currentImageData.height, metadata);

          // Surface the currently shown panel slice(s) in debug to avoid confusion when selected_slice_index is fixed.
          const debugSlice = currentDisplayedSlices[0] || currentImageData;
          const selectedStr = Number.isFinite(data.selected_slice_index)
            ? ` selected=${data.selected_slice_index}`
            : '';
          const fallbackDebugPrefix = usingCompatibilityFallback
            ? '!!! BACKWARD-COMPATIBILITY FALLBACK ACTIVE: using legacy single-slice fields (no valid slices[]) !!! '
            : '';
          updateStatus('debug', `${fallbackDebugPrefix}${formatHeaderPoseForDebug(debugSlice)} ${formatDisplayedSliceIndicesForDebug(currentDisplayedSlices)}${selectedStr}`);
          updateStatus('debugFallback', usingCompatibilityFallback
            ? 'UNIQUE_FALLBACK_DEBUG: ACTIVE (legacy single-slice fields in use)'
            : 'UNIQUE_FALLBACK_DEBUG: NOT_USED (per-slice slices[] path active)');
          
          const skippedCount = payloadSlices.length - renderableSlices.length;
          updateStatus('status2d', `✓ showing ${currentDisplayedSlices.length}/${currentSnapshotSlices.length} slices (${currentImageData.width}x${currentImageData.height})${skippedCount > 0 ? `; skipped ${skippedCount} invalid-geometry` : ''}`);
          return true;
        }
      }
    } catch (error) {
      console.error('Error loading 2D image:', error);
      updateStatus('status2d', `✗ ${error.message}`);
      updateStatus('debugFallback', 'UNIQUE_FALLBACK_DEBUG: UNKNOWN (2D fetch error)');
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

  // Fetch force sensing data from server (read_from5 = fileKey 4)
  async function updateForceSensingFromServer() {
    try {
      const response = await fetch(`${readServerUrl}/api/read/${clientId}/4`);
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      const data = await response.json();
      const ts = data.sent_at || data.received_at || data.timestamp || 0;

      if (ts && ts !== lastForceTimestamp) {
        lastForceTimestamp = ts;

        if (data.values && Array.isArray(data.values)) {
          currentForceSensingData = data;
          const v = data.values;
          const valuesStr = v.map(val => val.toFixed(3)).join(", ");
          updateStatus("forceSensing", `[${valuesStr}]`);
        }
      }
    } catch (error) {
      // Silently retry - force sensing data may not be available yet
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

          // Parse values into marker positions (every 3 values = one x,y,z point)
          fkControlPoints = parseFKControlPoints(v);
          const numPts = fkControlPoints.length;

          // Status: show value count and first few marker positions
          const valuesStr = v.slice(0, 12).map(val => val.toFixed(2)).join(", ")
                          + (v.length > 12 ? ` ... (${v.length} values total)` : "");
          updateStatus("fwdKinematics", `[${valuesStr}]`);

          const displayCount = Math.min(numPts, 5);
          const ptsStr = fkControlPoints.slice(0, displayCount).map((p, i) =>
            `P${i+1}(${p.x.toFixed(2)}, ${p.y.toFixed(2)}, ${p.z.toFixed(2)})`
          ).join(" | ") + (numPts > displayCount ? ` ... (${numPts} pts total)` : "");
          updateStatus("fkCtrlPts", `${numPts} pts: ${ptsStr}`);
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
      glMatrix.mat4.translate(baseMatrix, baseMatrix, [0.0, 0.0, volumeZoom]);
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
      // Free per-frame textures: this runs every animation frame, and
      // undeleted textures accumulate until the browser kills the WebGL
      // context (dead zoom/pan, "context lost" errors).
      gl.deleteTexture(sliceTexture);
    });
    
    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.enable(gl.CULL_FACE);
  }

  // Render only the latest 2D slice as a textured plane in the 3D canvas.
  function renderLatest2DSliceIn3D(gl, programInfo, imageData) {
    if (!imageData || !Array.isArray(imageData.values)) return;

    const { width, height, values, position, orientation, pixelSize } = imageData;
    const fieldOfView = (45 * Math.PI) / 180;
    const aspect = gl.canvas.clientWidth / gl.canvas.clientHeight;
    const projectionMatrix = glMatrix.mat4.create();
    glMatrix.mat4.perspective(projectionMatrix, fieldOfView, aspect, 0.1, 100.0);

    const getOrientationMatrix = (ori) => {
      if (!ori) return null;

      // Object form: { m00, m01, ..., m22 }
      if (Number.isFinite(ori.m00) && Number.isFinite(ori.m01) && Number.isFinite(ori.m02) &&
          Number.isFinite(ori.m10) && Number.isFinite(ori.m11) && Number.isFinite(ori.m12) &&
          Number.isFinite(ori.m20) && Number.isFinite(ori.m21) && Number.isFinite(ori.m22)) {
        return [
          ori.m00, ori.m01, ori.m02,
          ori.m10, ori.m11, ori.m12,
          ori.m20, ori.m21, ori.m22,
        ];
      }

      // Flat array form: [m00, m01, ..., m22]
      if (Array.isArray(ori) && ori.length === 9 && ori.every(Number.isFinite)) {
        return ori;
      }

      // Nested array form: [[m00,m01,m02], [m10,m11,m12], [m20,m21,m22]]
      if (Array.isArray(ori) && ori.length === 3 && ori.every(row => Array.isArray(row) && row.length === 3)) {
        const flat = [ori[0][0], ori[0][1], ori[0][2], ori[1][0], ori[1][1], ori[1][2], ori[2][0], ori[2][1], ori[2][2]];
        if (flat.every(Number.isFinite)) return flat;
      }

      return null;
    };

    const getPosition = (pos) => {
      if (!pos) return null;
      if (Array.isArray(pos) && pos.length === 3 && pos.every(Number.isFinite)) {
        return [pos[0], pos[1], pos[2]];
      }
      if (Number.isFinite(pos.x) && Number.isFinite(pos.y) && Number.isFinite(pos.z)) {
        return [pos.x, pos.y, pos.z];
      }
      return null;
    };

    const getPixelSize = (ps) => {
      if (!ps) return null;
      if (Array.isArray(ps) && ps.length === 2 && Number.isFinite(ps[0]) && Number.isFinite(ps[1])) {
        return [ps[0], ps[1]];
      }
      if (Number.isFinite(ps.x) && Number.isFinite(ps.y)) {
        return [ps.x, ps.y];
      }
      return null;
    };

    const pos = getPosition(position);
    const ori = getOrientationMatrix(orientation);
    const px = getPixelSize(pixelSize);

    const modelViewMatrix = glMatrix.mat4.create();
    glMatrix.mat4.translate(modelViewMatrix, modelViewMatrix, [0.0, 0.0, volumeZoom]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, mouseRotationX, [1, 0, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, mouseRotationY, [0, 1, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, mouseRotationZ, [0, 0, 1]);

    if (!(pos && ori && px && width > 0 && height > 0)) {
      // Strict mode: no metadata means no 3D slice-plane render.
      return;
    }

    // Pose matrix from metadata orientation and position.
    const poseMatrix = glMatrix.mat4.fromValues(
      ori[0], ori[3], ori[6], 0,
      ori[1], ori[4], ori[7], 0,
      ori[2], ori[5], ori[8], 0,
      pos[0], pos[1], pos[2], 1
    );

    // Quad is [-1,1] in each axis, so scale by half extents in world units.
    const halfWidthWorld = 0.5 * width * px[0];
    const halfHeightWorld = 0.5 * height * px[1];
    const scaleMatrix = glMatrix.mat4.create();
    glMatrix.mat4.scale(scaleMatrix, scaleMatrix, [halfWidthWorld, halfHeightWorld, 1.0]);

    const planeModel = glMatrix.mat4.create();
    glMatrix.mat4.multiply(planeModel, poseMatrix, scaleMatrix);

    const combined = glMatrix.mat4.create();
    glMatrix.mat4.multiply(combined, modelViewMatrix, planeModel);
    glMatrix.mat4.copy(modelViewMatrix, combined);

    const sliceTexture = createTextureFromMatrix(gl, values, width, height);
    renderQuad(gl, programInfo, sliceTexture, projectionMatrix, modelViewMatrix);
    // Free per-frame textures (see renderVolumeCube note): leak here killed
    // the WebGL context during the 2026-08-18 scanner test.
    gl.deleteTexture(sliceTexture);
  }

  // Render the latest 3 slices as planes in 3D, using each image header's
  // position/orientation metadata for placement.
  function renderSliceHistoryIn3D(gl, programInfo, imageHistory) {
    if (!Array.isArray(imageHistory) || imageHistory.length === 0) return false;

    const getOrientationMatrix = (ori) => {
      if (!ori) return null;
      if (Number.isFinite(ori.m00) && Number.isFinite(ori.m01) && Number.isFinite(ori.m02) &&
          Number.isFinite(ori.m10) && Number.isFinite(ori.m11) && Number.isFinite(ori.m12) &&
          Number.isFinite(ori.m20) && Number.isFinite(ori.m21) && Number.isFinite(ori.m22)) {
        return [
          ori.m00, ori.m01, ori.m02,
          ori.m10, ori.m11, ori.m12,
          ori.m20, ori.m21, ori.m22,
        ];
      }
      if (Array.isArray(ori) && ori.length === 9 && ori.every(Number.isFinite)) return ori;
      if (Array.isArray(ori) && ori.length === 3 && ori.every(row => Array.isArray(row) && row.length === 3)) {
        const flat = [ori[0][0], ori[0][1], ori[0][2], ori[1][0], ori[1][1], ori[1][2], ori[2][0], ori[2][1], ori[2][2]];
        if (flat.every(Number.isFinite)) return flat;
      }
      return null;
    };

    const getPosition = (pos) => {
      if (!pos) return null;
      if (Array.isArray(pos) && pos.length === 3 && pos.every(Number.isFinite)) return [pos[0], pos[1], pos[2]];
      if (Number.isFinite(pos.x) && Number.isFinite(pos.y) && Number.isFinite(pos.z)) return [pos.x, pos.y, pos.z];
      return null;
    };

    const getPixelSize = (ps) => {
      if (!ps) return null;
      if (Array.isArray(ps) && ps.length === 2 && Number.isFinite(ps[0]) && Number.isFinite(ps[1])) return [ps[0], ps[1]];
      if (Number.isFinite(ps.x) && Number.isFinite(ps.y)) return [ps.x, ps.y];
      return null;
    };

    const slices = imageHistory.slice(0, 3)
      .map((img) => {
        if (!img || !Array.isArray(img.values) || !img.width || !img.height) return null;
        const pos = getPosition(img.position);
        const ori = getOrientationMatrix(img.orientation);
        const px = getPixelSize(img.pixelSize);
        if (!(pos && ori && px)) return null;
        return {
          values: img.values,
          width: img.width,
          height: img.height,
          pos,
          ori,
          px,
        };
      })
      .filter(Boolean);

    if (slices.length === 0) return false;

    let cx = 0, cy = 0, cz = 0;
    for (const s of slices) {
      cx += s.pos[0];
      cy += s.pos[1];
      cz += s.pos[2];
    }
    cx /= slices.length;
    cy /= slices.length;
    cz /= slices.length;

    const fieldOfView = (45 * Math.PI) / 180;
    const aspect = gl.canvas.clientWidth / gl.canvas.clientHeight;
    const projectionMatrix = glMatrix.mat4.create();
    glMatrix.mat4.perspective(projectionMatrix, fieldOfView, aspect, 0.1, 300.0);

    const viewMatrix = glMatrix.mat4.create();
    glMatrix.mat4.translate(viewMatrix, viewMatrix, [0.0, 0.0, sliceHistoryZoom]);
    glMatrix.mat4.rotate(viewMatrix, viewMatrix, sliceHistoryRotationX, [1, 0, 0]);
    glMatrix.mat4.rotate(viewMatrix, viewMatrix, sliceHistoryRotationY, [0, 1, 0]);
    glMatrix.mat4.rotate(viewMatrix, viewMatrix, sliceHistoryRotationZ, [0, 0, 1]);

    gl.disable(gl.CULL_FACE);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.enable(gl.DEPTH_TEST);

    for (const s of slices) {
      const localPos = [s.pos[0] - cx, s.pos[1] - cy, s.pos[2] - cz];

      const poseMatrix = glMatrix.mat4.fromValues(
        s.ori[0], s.ori[3], s.ori[6], 0,
        s.ori[1], s.ori[4], s.ori[7], 0,
        s.ori[2], s.ori[5], s.ori[8], 0,
        localPos[0], localPos[1], localPos[2], 1
      );

      const halfWidthWorld = 0.5 * s.width * s.px[0];
      const halfHeightWorld = 0.5 * s.height * s.px[1];
      const scaleMatrix = glMatrix.mat4.create();
      glMatrix.mat4.scale(scaleMatrix, scaleMatrix, [halfWidthWorld, halfHeightWorld, 1.0]);

      const planeModel = glMatrix.mat4.create();
      glMatrix.mat4.multiply(planeModel, poseMatrix, scaleMatrix);

      const modelViewMatrix = glMatrix.mat4.create();
      glMatrix.mat4.multiply(modelViewMatrix, viewMatrix, planeModel);

      const sliceTexture = createTextureFromMatrix(gl, s.values, s.width, s.height);
      renderQuad(gl, programInfo, sliceTexture, projectionMatrix, modelViewMatrix);
      // Free per-frame textures (see renderVolumeCube note).
      gl.deleteTexture(sliceTexture);
    }

    gl.disable(gl.BLEND);
    gl.enable(gl.CULL_FACE);
    return true;
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

  // Mesh shader: simple position + flat color (renders on same gl context as volume)
  const meshVsSource = `
    attribute vec3 aMeshPosition;
    uniform mat4 uModelViewMatrix;
    uniform mat4 uProjectionMatrix;
    void main(void) {
      gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aMeshPosition, 1.0);
    }
  `;

  const meshFsSource = `
    precision mediump float;
    uniform vec4 uColor;
    void main(void) {
      gl_FragColor = uColor;
    }
  `;

  const meshShaderProgram = initShaderProgram(gl, meshVsSource, meshFsSource);
  const meshProgramInfo = {
    program: meshShaderProgram,
    attribLocations: {
      position: gl.getAttribLocation(meshShaderProgram, "aMeshPosition"),
    },
    uniformLocations: {
      projectionMatrix: gl.getUniformLocation(meshShaderProgram, "uProjectionMatrix"),
      modelViewMatrix: gl.getUniformLocation(meshShaderProgram, "uModelViewMatrix"),
      color: gl.getUniformLocation(meshShaderProgram, "uColor"),
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

  // Catmull-Rom spline interpolation between ALL control points
  // For each segment Pi -> Pi+1, we use points Pi-1, Pi, Pi+1, Pi+2
  // At the boundaries, we clamp: P-1 = P0 and PN+1 = PN
  // This ensures the spline passes through every control point
  function catmullRomPoint(p0, p1, p2, p3, t) {
    const t2 = t * t;
    const t3 = t2 * t;
    return {
      x: 0.5 * ((2 * p1.x) + (-p0.x + p2.x) * t + (2*p0.x - 5*p1.x + 4*p2.x - p3.x) * t2 + (-p0.x + 3*p1.x - 3*p2.x + p3.x) * t3),
      y: 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t + (2*p0.y - 5*p1.y + 4*p2.y - p3.y) * t2 + (-p0.y + 3*p1.y - 3*p2.y + p3.y) * t3),
      z: 0.5 * ((2 * p1.z) + (-p0.z + p2.z) * t + (2*p0.z - 5*p1.z + 4*p2.z - p3.z) * t2 + (-p0.z + 3*p1.z - 3*p2.z + p3.z) * t3),
    };
  }

  // Generate interpolated spline points through all control points
  // segments: number of interpolated points per segment between consecutive control points
  function generateSplinePoints(controlPoints, segments = 20) {
    if (controlPoints.length < 2) return [...controlPoints];
    const spline = [];
    const n = controlPoints.length;
    for (let i = 0; i < n - 1; i++) {
      // Clamp indices at boundaries so spline passes through P0 and P(n-1)
      const p0 = controlPoints[Math.max(i - 1, 0)];
      const p1 = controlPoints[i];
      const p2 = controlPoints[i + 1];
      const p3 = controlPoints[Math.min(i + 2, n - 1)];
      for (let s = 0; s < segments; s++) {
        const t = s / segments;
        spline.push(catmullRomPoint(p0, p1, p2, p3, t));
      }
    }
    // Add the last control point
    spline.push(controlPoints[n - 1]);
    return spline;
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
    glMatrix.mat4.translate(modelViewMatrix, modelViewMatrix, [0.0, 0.0, fkZoom]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, fkMouseRotationX, [1, 0, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, fkMouseRotationY, [0, 1, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, fkMouseRotationZ, [0, 0, 1]);

    // Compute centroid and scale from the FIRST frame only, then reuse
    // This prevents moving control points from shifting the entire view
    if (!fkFixedCentroid) {
      let cx = 0, cy = 0, cz = 0;
      for (const p of controlPoints) { cx += p.x; cy += p.y; cz += p.z; }
      cx /= controlPoints.length;
      cy /= controlPoints.length;
      cz /= controlPoints.length;
      fkFixedCentroid = { x: cx, y: cy, z: cz };

      let maxDist = 0.001;
      for (const p of controlPoints) {
        const dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
        const dist = Math.sqrt(dx*dx + dy*dy + dz*dz);
        if (dist > maxDist) maxDist = dist;
      }
      fkFixedScale = 6.0 / maxDist;
    }
    const cx = fkFixedCentroid.x;
    const cy = fkFixedCentroid.y;
    const cz = fkFixedCentroid.z;
    const scale = fkFixedScale;

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

    // Draw catheter curve through all marker positions.
    // If there are many markers (CRM FK output), render them directly as LINE_STRIP
    // so the dense CRM-computed shape is shown exactly.
    // If there are only a few markers (legacy sparse control points), apply
    // Catmull-Rom spline interpolation to produce a smooth curve.
    const MAX_SPARSE_POINTS = 10;  // threshold: above this, use direct LINE_STRIP rendering
    if (numPts >= 2) {
      const useCatmullRom = numPts <= MAX_SPARSE_POINTS;
      const linePoints = useCatmullRom ? generateSplinePoints(controlPoints, 20) : controlPoints;

      const splinePositions = [];
      const splineColors = [];
      for (const sp of linePoints) {
        splinePositions.push((sp.x - cx) * scale, (sp.y - cy) * scale, (sp.z - cz) * scale);
        splineColors.push(0.8, 0.8, 0.8); // Light gray catheter curve
      }

      const splinePosBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, splinePosBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(splinePositions), gl.STATIC_DRAW);
      gl.vertexAttribPointer(programInfo.attribLocations.position, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(programInfo.attribLocations.position);

      const splineColBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, splineColBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(splineColors), gl.STATIC_DRAW);
      gl.vertexAttribPointer(programInfo.attribLocations.color, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(programInfo.attribLocations.color);

      gl.uniform1f(programInfo.uniformLocations.pointSize, 1.0);
      gl.lineWidth(2.0);
      gl.drawArrays(gl.LINE_STRIP, 0, linePoints.length);

      // Free per-frame buffers (see renderQuad note).
      gl.deleteBuffer(splinePosBuffer);
      gl.deleteBuffer(splineColBuffer);
    }

    // Draw marker points on top of the curve — all points, no subsampling.
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

    // Free per-frame buffers (see renderQuad note).
    gl.deleteBuffer(posBuffer);
    gl.deleteBuffer(colBuffer);
    gl.deleteBuffer(axisPosBuffer);
    gl.deleteBuffer(axisColBuffer);
  }

  // Render OFF surface mesh overlaid on the 3D volume canvas
  function renderMesh(gl, programInfo, meshBuffers) {
    if (!meshBuffers) return;

    const fieldOfView = (45 * Math.PI) / 180;
    const aspect = gl.canvas.clientWidth / gl.canvas.clientHeight;
    const projectionMatrix = glMatrix.mat4.create();
    glMatrix.mat4.perspective(projectionMatrix, fieldOfView, aspect, 0.1, 100.0);

    const modelViewMatrix = glMatrix.mat4.create();
    glMatrix.mat4.translate(modelViewMatrix, modelViewMatrix, [0.0, 0.0, volumeZoom]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, mouseRotationX, [1, 0, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, mouseRotationY, [0, 1, 0]);
    glMatrix.mat4.rotate(modelViewMatrix, modelViewMatrix, mouseRotationZ, [0, 0, 1]);
    glMatrix.mat4.scale(modelViewMatrix, modelViewMatrix, [2.5, 2.5, 2.5]);

    gl.useProgram(programInfo.program);
    gl.uniformMatrix4fv(programInfo.uniformLocations.projectionMatrix, false, projectionMatrix);
    gl.uniformMatrix4fv(programInfo.uniformLocations.modelViewMatrix, false, modelViewMatrix);
    gl.uniform4fv(programInfo.uniformLocations.color, [0.3, 0.8, 1.0, 0.2]);

    gl.bindBuffer(gl.ARRAY_BUFFER, meshBuffers.position);
    gl.vertexAttribPointer(programInfo.attribLocations.position, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(programInfo.attribLocations.position);

    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, meshBuffers.indices);

    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(false);

    gl.drawElements(gl.TRIANGLES, meshBuffers.indexCount, gl.UNSIGNED_SHORT, 0);

    gl.uniform4fv(programInfo.uniformLocations.color, [0.0, 0.95, 1.0, 1.0]);
    gl.disable(gl.DEPTH_TEST);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, meshBuffers.edgeIndices);
    gl.drawElements(gl.LINES, meshBuffers.edgeIndexCount, gl.UNSIGNED_SHORT, 0);

    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.enable(gl.DEPTH_TEST);
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

  canvasFK.addEventListener('wheel', (e) => {
    e.preventDefault();
    fkZoom += e.deltaY * 0.05;  // Scroll up = zoom in, scroll down = zoom out
    fkZoom = Math.min(-2.0, Math.max(-100.0, fkZoom));  // Clamp range
  }, { passive: false });

  // Actuation current sliders (I1–I6): update displayed value on change
  for (let i = 1; i <= 6; i++) {
    const slider = document.getElementById(`sliderI${i}`);
    const valSpan = document.getElementById(`valI${i}`);
    if (slider && valSpan) {
      slider.addEventListener('input', () => {
        valSpan.textContent = parseFloat(slider.value).toFixed(2);
      });
    }
  }

  // Read current slider values for I1–I6
  function getCurrentSliderValues() {
    const vals = [];
    for (let i = 1; i <= 6; i++) {
      const slider = document.getElementById(`sliderI${i}`);
      vals.push(slider ? parseFloat(slider.value) : 0.0);
    }
    return vals;
  }

  // POST mode value to server (always includes mode; includes currents when manual)
  async function postModeToServer(modeValue) {
    try {
      const values = [modeValue];
      if (modeValue === -1) {
        values.push(...getCurrentSliderValues());
      }
      const payload = {
        client_id: clientId,
        sent_at: Date.now(),
        values: values
      };
      const response = await fetch(`${writeServerUrl}/api/write/${clientId}/0`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    } catch (error) {
      console.error('Error posting mode value:', error);
    }
  }

  // Mode slider: -1 = Manual Control, 0 = OFF, 1 = Planning Control
  const modeSlider = document.getElementById('sliderMode');
  const modeLabel = document.getElementById('valMode');
  
  function updateModeLabel(val) {
    if (!modeLabel) return;
    if (val === -1) {
      modeLabel.textContent = 'Manual Control';
      modeLabel.style.color = '#ff9900';
    } else if (val === 1) {
      modeLabel.textContent = 'Planning Control';
      modeLabel.style.color = '#51cf66';
    } else {
      modeLabel.textContent = 'OFF';
      modeLabel.style.color = '#ff6b6b';
    }
  }

  if (modeSlider && modeLabel) {
    modeSlider.value = '0';
    updateModeLabel(0);
    postModeToServer(0);
    // Write immediately when mode slider changes
    modeSlider.addEventListener('input', () => {
      const val = parseInt(modeSlider.value);
      updateModeLabel(val);
      postModeToServer(val);
    });

    // Write immediately when any current slider changes while in Manual mode
    for (let i = 1; i <= 6; i++) {
      const slider = document.getElementById(`sliderI${i}`);
      if (slider) {
        slider.addEventListener('input', () => {
          if (parseInt(modeSlider.value) === -1) {
            postModeToServer(-1);
          }
        });
      }
    }

    // 20Hz: always post mode; includes currents when mode is Manual (-1)
    setInterval(() => {
      const val = parseInt(modeSlider.value);
      postModeToServer(val);
    }, 40); //50->40 
  }

  const slicePlusBtn = document.getElementById('slicePlusBtn');
  const sliceMinusBtn = document.getElementById('sliceMinusBtn');
  const savePositionButtons = [
    document.getElementById('savePositionBtn1'),
    document.getElementById('savePositionBtn2'),
    document.getElementById('savePositionBtn3')
  ];
  const sendAbsolutePositionButtons = [
    document.getElementById('sendAbsolutePositionBtn1'),
    document.getElementById('sendAbsolutePositionBtn2'),
    document.getElementById('sendAbsolutePositionBtn3')
  ];
  const rotXSlider = document.getElementById('sliderRotX');
  const rotYSlider = document.getElementById('sliderRotY');
  const rotZSlider = document.getElementById('sliderRotZ');
  const sliceThicknessSlider = document.getElementById('sliderSliceThickness');
  const rotXVal = document.getElementById('valRotX');
  const rotYVal = document.getElementById('valRotY');
  const rotZVal = document.getElementById('valRotZ');
  const sliceThicknessVal = document.getElementById('valSliceThickness');

  if (slicePlusBtn) {
    slicePlusBtn.addEventListener('click', () => {
      postSliceTranslationToServer(1);
    });
  }

  if (sliceMinusBtn) {
    sliceMinusBtn.addEventListener('click', () => {
      postSliceTranslationToServer(-1);
    });
  }

  for (let i = 0; i < 3; i++) {
    const saveBtn = savePositionButtons[i];
    const sendBtn = sendAbsolutePositionButtons[i];

    if (saveBtn) {
      saveBtn.addEventListener('click', async () => {
        await postSavedTransformToServer(i, currentImageData);
      });
    }

    if (sendBtn) {
      sendBtn.disabled = !savedTransformDataBySlot[i];
      sendBtn.addEventListener('click', async () => {
        await postSavedTransformToSliceTarget(i, savedTransformDataBySlot[i]);
      });
    }
  }

  if (sliceThicknessSlider && sliceThicknessVal) {
    sliceThicknessVal.textContent = sliceThicknessSlider.value;

    sliceThicknessSlider.addEventListener('input', () => {
      sliceThicknessVal.textContent = sliceThicknessSlider.value;
    });

    sliceThicknessSlider.addEventListener('change', () => {
      const thickness = parseInt(sliceThicknessSlider.value, 10);
      postSliceThicknessToServer(thickness);
    });
  }

  if (rotXSlider) {
    rotXSlider.addEventListener('change', async () => {
      const deg = parseInt(rotXSlider.value, 10);
      await postRotationDeltaToPoseTransform('x', deg);
      if (Number.isFinite(deg) && rotXVal) {
        rotXVal.dataset.lastApplied = String(deg);
        rotXVal.textContent = `0 (last ${deg})`;
      }
      rotXSlider.value = '0';
    });
  }

  if (rotYSlider) {
    rotYSlider.addEventListener('change', async () => {
      const deg = parseInt(rotYSlider.value, 10);
      await postRotationDeltaToPoseTransform('y', deg);
      if (Number.isFinite(deg) && rotYVal) {
        rotYVal.dataset.lastApplied = String(deg);
        rotYVal.textContent = `0 (last ${deg})`;
      }
      rotYSlider.value = '0';
    });
  }

  if (rotZSlider) {
    rotZSlider.addEventListener('change', async () => {
      const deg = parseInt(rotZSlider.value, 10);
      await postRotationDeltaToPoseTransform('z', deg);
      if (Number.isFinite(deg) && rotZVal) {
        rotZVal.dataset.lastApplied = String(deg);
        rotZVal.textContent = `0 (last ${deg})`;
      }
      rotZSlider.value = '0';
    });
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

  function normalizeWheelDelta(e) {
    // deltaMode: 0=pixels, 1=lines, 2=pages
    if (e.deltaMode === 1) return e.deltaY * 16;
    if (e.deltaMode === 2) return e.deltaY * 120;
    return e.deltaY;
  }

  canvas3D.addEventListener('wheel', (e) => {
    e.preventDefault();
    const delta = normalizeWheelDelta(e);
    volumeZoom += delta * 0.04;
    volumeZoom = Math.min(-1.5, Math.max(-120.0, volumeZoom));
    updateStatus('debug', `Vol zoom ${volumeZoom.toFixed(1)} | Slice zoom ${sliceHistoryZoom.toFixed(1)}`);
  }, { passive: false });

  if (canvasSlices3D) {
    canvasSlices3D.addEventListener('mousedown', (e) => {
      isSliceHistoryMouseDown = true;
      lastSliceHistoryMouseX = e.clientX;
      lastSliceHistoryMouseY = e.clientY;
      e.preventDefault();
    });

    canvasSlices3D.addEventListener('mousemove', (e) => {
      if (isSliceHistoryMouseDown) {
        const deltaX = e.clientX - lastSliceHistoryMouseX;
        const deltaY = e.clientY - lastSliceHistoryMouseY;
        const sensitivity = 0.01;
        sliceHistoryRotationY += deltaX * sensitivity;
        sliceHistoryRotationX += deltaY * sensitivity;
        if (e.shiftKey) {
          sliceHistoryRotationZ += deltaX * sensitivity;
        }
        lastSliceHistoryMouseX = e.clientX;
        lastSliceHistoryMouseY = e.clientY;
      }
    });

    canvasSlices3D.addEventListener('mouseup', () => {
      isSliceHistoryMouseDown = false;
    });

    canvasSlices3D.addEventListener('mouseleave', () => {
      isSliceHistoryMouseDown = false;
    });

    canvasSlices3D.addEventListener('wheel', (e) => {
      e.preventDefault();
      const delta = normalizeWheelDelta(e);
      sliceHistoryZoom += delta * 0.2;
      sliceHistoryZoom = Math.min(-5.0, Math.max(-600.0, sliceHistoryZoom));
      updateStatus('debug', `Vol zoom ${volumeZoom.toFixed(1)} | Slice zoom ${sliceHistoryZoom.toFixed(1)}`);
    }, { passive: false });
  }

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

  // Load chest surface mesh from static file (currently disabled)
  let meshBuffers = null;
  async function loadMesh() {
    const meshUrls = [
      `${readServerUrl}/1_0_chest_surface.off`,
      '/1_0_chest_surface.off',
      './1_0_chest_surface.off',
    ];

    let lastError = null;
    for (const meshUrl of meshUrls) {
      try {
        const response = await fetch(meshUrl, { cache: 'no-store' });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        const text = await response.text();
        const { positions, indices } = parseOFFMesh(text);
        meshBuffers = createMeshBuffers(gl, positions, indices);
        updateStatus('status3d', `✓ volume + mesh (${positions.length / 3}v, ${indices.length / 3}f)`);
        console.log(`✓ Mesh loaded from ${meshUrl}: ${positions.length / 3} vertices, ${indices.length / 3} faces`);
        return;
      } catch (err) {
        lastError = err;
      }
    }

    console.error('Error loading mesh from all candidates:', lastError);
    updateStatus('status3d', `✗ mesh load failed (${lastError ? lastError.message : 'unknown'})`);
  }

  // Mesh loading disabled to avoid startup 404s when OFF file is unavailable.
  // await loadMesh();

  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);

  let then = 0;

  // Throttle flags: prevent new fetch if previous is still in-flight
  let fetchingTexture = false;
  let fetchingVolume = false;
  let fetchingTipPose = false;
  let fetchingFK = false;
  let fetchingForceSensing = false;

  function render(now) {
    now *= 0.001;
    deltaTime = now - then;
    then = now;

    // Each fetch runs independently; a slow fetch won't block the others
    
    if (!fetchingTexture) {
      postWillUpdateTextureFromServerToServer(currentImageData);
      fetchingTexture = true;
      
      updateTextureFromServer().catch(() => {}).finally(() => {
        fetchingTexture = false;
        postUpdatedTextureFromServerToServer(currentImageData);
      });
    }
    if (!fetchingVolume) {
      fetchingVolume = true;
      updateVolumeFromServer().catch(() => {}).finally(() => { fetchingVolume = false; });
    }
    if (!fetchingTipPose) {
      fetchingTipPose = true;
      updateTipPoseFromServer().catch(() => {}).finally(() => { fetchingTipPose = false; });
    }
    if (!fetchingFK) {
      fetchingFK = true;
      updateForwardKinematicsFromServer().catch(() => {}).finally(() => { fetchingFK = false; });
    }
    if (!fetchingForceSensing) {
      fetchingForceSensing = true;
      postWillUpdateForceSensingFromServerToServer(currentForceSensingData);
      updateForceSensingFromServer().catch(() => {}).finally(() => {
        fetchingForceSensing = false;
        postUpdatedForceSensingFromServerToServer(currentForceSensingData);
      });
    }

    gl.clearColor(0.0, 0.0, 0.0, 1.0);
    gl.clearDepth(1.0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // renderVolumeCube(gl, programInfo, volumeSlices);
    renderLatest2DSliceIn3D(gl, programInfo, currentImageData);
    // renderMesh(gl, meshProgramInfo, meshBuffers);

    if (glSlices && sliceProgramInfo) {
      glSlices.clearColor(0.0, 0.0, 0.0, 1.0);
      glSlices.clearDepth(1.0);
      glSlices.enable(glSlices.DEPTH_TEST);
      glSlices.depthFunc(glSlices.LEQUAL);
      glSlices.clear(glSlices.COLOR_BUFFER_BIT | glSlices.DEPTH_BUFFER_BIT);
      const rendered = renderSliceHistoryIn3D(glSlices, sliceProgramInfo, currentImageHistory);
      updateStatus('status3dSlices', rendered ? `✓ last ${Math.min(currentImageHistory.length, 3)} slices in 3D` : 'Waiting for slice metadata...');
    }

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

// Parse an ASCII .OFF mesh file and return normalized Float32Array positions + Uint16Array indices.
// Vertices are centered at their centroid and scaled to fit within a unit sphere (radius 1.0).
function parseOFFMesh(text) {
  const lines = text.trim().split('\n').filter(l => l.trim() && !l.trim().startsWith('#'));
  let i = 0;
  if (lines[i].trim() === 'OFF') i++;
  const header = lines[i++].trim().split(/\s+/).map(Number);
  const nVerts = header[0];
  const nFaces = header[1];

  const rawPositions = new Float32Array(nVerts * 3);
  for (let v = 0; v < nVerts; v++, i++) {
    const parts = lines[i].trim().split(/\s+/);
    rawPositions[v * 3]     = parseFloat(parts[0]);
    rawPositions[v * 3 + 1] = parseFloat(parts[1]);
    rawPositions[v * 3 + 2] = parseFloat(parts[2]);
  }

  // Compute centroid
  let cx = 0, cy = 0, cz = 0;
  for (let v = 0; v < nVerts; v++) {
    cx += rawPositions[v * 3];
    cy += rawPositions[v * 3 + 1];
    cz += rawPositions[v * 3 + 2];
  }
  cx /= nVerts; cy /= nVerts; cz /= nVerts;

  // Compute bounding radius and scale to unit sphere
  let maxDist = 0.001;
  for (let v = 0; v < nVerts; v++) {
    const dx = rawPositions[v * 3] - cx;
    const dy = rawPositions[v * 3 + 1] - cy;
    const dz = rawPositions[v * 3 + 2] - cz;
    const d = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (d > maxDist) maxDist = d;
  }
  const scale = 1.0 / maxDist;

  const positions = new Float32Array(nVerts * 3);
  for (let v = 0; v < nVerts; v++) {
    positions[v * 3]     = (rawPositions[v * 3]     - cx) * scale;
    positions[v * 3 + 1] = (rawPositions[v * 3 + 1] - cy) * scale;
    positions[v * 3 + 2] = (rawPositions[v * 3 + 2] - cz) * scale;
  }

  const indices = new Uint16Array(nFaces * 3);
  let k = 0;
  for (let f = 0; f < nFaces; f++, i++) {
    const parts = lines[i].trim().split(/\s+/).map(Number);
    // parts[0] = vertex count (3 for triangle), parts[1..3] = indices
    indices[k++] = parts[1];
    indices[k++] = parts[2];
    indices[k++] = parts[3];
  }

  return { positions, indices };
}

// Upload parsed mesh data to WebGL buffers
function createMeshBuffers(gl, positions, indices) {
  const posBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, positions, gl.STATIC_DRAW);

  const idxBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);

  const edgeSet = new Set();
  const edgeIndices = [];
  for (let i = 0; i < indices.length; i += 3) {
    const tri = [indices[i], indices[i + 1], indices[i + 2]];
    for (let j = 0; j < 3; j++) {
      const a = tri[j];
      const b = tri[(j + 1) % 3];
      const lo = Math.min(a, b);
      const hi = Math.max(a, b);
      const key = `${lo}:${hi}`;
      if (!edgeSet.has(key)) {
        edgeSet.add(key);
        edgeIndices.push(a, b);
      }
    }
  }

  const edgeIdxBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, edgeIdxBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(edgeIndices), gl.STATIC_DRAW);

  return {
    position: posBuffer,
    indices: idxBuffer,
    indexCount: indices.length,
    edgeIndices: edgeIdxBuffer,
    edgeIndexCount: edgeIndices.length,
  };
}