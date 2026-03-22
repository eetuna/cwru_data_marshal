// Dedicated 2D image renderer - renders full-screen image with simple orthographic projection
// Left half of screen = left half of canvas = exact 1:1 pixel mapping

export class Image2DRenderer {
  constructor(gl) {
    this.gl = gl;
    this.programInfo = this.createShaderProgram();
    this.texture = null;
    this.imageData = null;
    this.metadata = null; // Store position, orientation, pixelSize
  }

  createShaderProgram() {
    const gl = this.gl;
    
    const vsSource = `
      attribute vec4 aVertexPosition;
      attribute vec2 aTextureCoord;
      uniform mat4 uProjectionMatrix;
      uniform mat4 uModelViewMatrix;
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

    const shaderProgram = this.initShaderProgram(vsSource, fsSource);
    
    return {
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
  }

  initShaderProgram(vsSource, fsSource) {
    const gl = this.gl;
    const vertexShader = this.loadShader(gl.VERTEX_SHADER, vsSource);
    const fragmentShader = this.loadShader(gl.FRAGMENT_SHADER, fsSource);

    const shaderProgram = gl.createProgram();
    gl.attachShader(shaderProgram, vertexShader);
    gl.attachShader(shaderProgram, fragmentShader);
    gl.linkProgram(shaderProgram);

    if (!gl.getProgramParameter(shaderProgram, gl.LINK_STATUS)) {
      console.error('Shader program linking failed:', gl.getProgramInfoLog(shaderProgram));
      return null;
    }

    return shaderProgram;
  }

  loadShader(type, source) {
    const gl = this.gl;
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      console.error('Shader compilation failed:', gl.getShaderInfoLog(shader));
      gl.deleteShader(shader);
      return null;
    }

    return shader;
  }

  createTextureFromData(data, width, height) {
    const gl = this.gl;
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);

    let minValue = Infinity;
    let maxValue = -Infinity;
    
    for (let i = 0; i < data.length; i++) {
      if (data[i] < minValue) minValue = data[i];
      if (data[i] > maxValue) maxValue = data[i];
    }
    
    const range = maxValue - minValue > 0.001 ? maxValue - minValue : 1.0;
    const rgbaData = new Uint8Array(width * height * 4);
    
    for (let i = 0; i < data.length; i++) {
      const normalized = (data[i] - minValue) / range;
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

  updateImage(data, width, height, metadata = null) {
    const floatData = new Float32Array(data);
    this.texture = this.createTextureFromData(floatData, width, height);
    this.imageData = { width, height, values: data };
    this.metadata = metadata; // Store metadata if provided
    console.log(`[2D Image] Updated texture: ${width}x${height}`);
    if (metadata) {
      console.log(`[2D Image] Metadata - Position:`, metadata.position, `Orientation:`, metadata.orientation, `PixelSize:`, metadata.pixelSize);
    }
  }

  render(canvasWidth, canvasHeight) {
    if (!this.texture) return;

    const gl = this.gl;
    const programInfo = this.programInfo;

    gl.useProgram(programInfo.program);

    // Orthographic projection: left half of screen = [0, canvasWidth/2] x [0, canvasHeight]
    // Map to NDC: [-1, 0] x [-1, 1]
    const projectionMatrix = glMatrix.mat4.create();
    glMatrix.mat4.ortho(
      projectionMatrix,
      -1, 0,      // left, right (left half)
      -1, 1,      // bottom, top
      0.1, 100.0  // near, far
    );

    const modelViewMatrix = glMatrix.mat4.create();
    glMatrix.mat4.translate(modelViewMatrix, modelViewMatrix, [-0.5, 0.0, -1.0]);

    gl.uniformMatrix4fv(programInfo.uniformLocations.projectionMatrix, false, projectionMatrix);
    gl.uniformMatrix4fv(programInfo.uniformLocations.modelViewMatrix, false, modelViewMatrix);

    // Simple quad covering left half
    const quadPositions = [
      -1.0, -1.0, 0.0,
       0.0, -1.0, 0.0,
       0.0,  1.0, 0.0,
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

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.uniform1i(programInfo.uniformLocations.uSampler, 0);

    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
  }

  // Convert screen click to image pixel coordinates
  getPixelAtClick(screenX, screenY, canvasWidth, canvasHeight) {
    if (!this.imageData) return null;

    const { width, height, values } = this.imageData;
    const leftHalfWidth = canvasWidth / 2;

    // Check if click is in left half
    if (screenX >= leftHalfWidth) {
      console.log(`[2D Click] Outside left half (x=${screenX})`);
      return null;
    }

    // Simple linear mapping: screen pixel -> image pixel
    const pixelX = Math.floor((screenX / leftHalfWidth) * width);
    const pixelY = Math.floor((screenY / canvasHeight) * height);

    // Clamp to valid range
    const x = Math.max(0, Math.min(width - 1, pixelX));
    const y = Math.max(0, Math.min(height - 1, pixelY));

    const index = y * width + x;
    const pixelValue = values[index];

    console.log(`[2D Click] Screen (${screenX.toFixed(0)}, ${screenY.toFixed(0)}) -> Image pixel [${x}, ${y}], value=${pixelValue.toFixed(4)}`);

    return {
      pixelX: x,
      pixelY: y,
      value: pixelValue
    };
  }

  // Convert pixel coordinates to world coordinates using metadata
  pixelToWorldCoordinates(pixelX, pixelY) {
    if (!this.metadata) {
      console.warn('[2D Image] No metadata available for coordinate transformation');
      return null;
    }

    const { position, orientation, pixelSize } = this.metadata;

    if (!position || !orientation || !pixelSize) {
      console.warn('[2D Image] Incomplete metadata for coordinate transformation');
      return null;
    }

    // Convert pixel coordinates to image plane coordinates (centered at origin)
    // Assuming the image plane is centered with respect to the pixel grid
    const { width, height } = this.imageData;
    const imageX = (pixelX - width / 2) * pixelSize.x;
    const imageY = (pixelY - height / 2) * pixelSize.y;
    const imageZ = 0; // Pixels are on the z=0 plane of the image

    // Apply orientation rotation to convert from image plane to world coordinates
    // orientation contains rotation matrix components: m00, m01, m02, m10, m11, m12, m20, m21, m22
    const worldX = 
      orientation.m00 * imageX + orientation.m01 * imageY + orientation.m02 * imageZ + position.x;
    const worldY = 
      orientation.m10 * imageX + orientation.m11 * imageY + orientation.m12 * imageZ + position.y;
    const worldZ = 
      orientation.m20 * imageX + orientation.m21 * imageY + orientation.m22 * imageZ + position.z;

    console.log(`[2D Image] Pixel [${pixelX}, ${pixelY}] -> World (${worldX.toFixed(2)}, ${worldY.toFixed(2)}, ${worldZ.toFixed(2)})`);

    return {
      x: worldX,
      y: worldY,
      z: worldZ,
      pixelX: pixelX,
      pixelY: pixelY
    };
  }
}
