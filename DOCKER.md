# Running CasparCG Server in Docker

This document provides instructions for building and running CasparCG Server in Docker with proper OpenGL support.

## About CasparCG Server

CasparCG Server is a professional broadcast graphics and video playout server that communicates via the **AMCP (Advanced Media Control Protocol)** on ports 5250-5252. It's designed to run headless in production environments with full OpenGL acceleration for video mixing and graphics rendering.

**This Dockerfile is based on the existing working Docker configuration** from `tools/linux/Dockerfile` but optimized for Coolify deployment with proper headless OpenGL support.

## Prerequisites

- Docker 20.10+ with BuildKit enabled
- NVIDIA Docker runtime (for GPU acceleration)
- DeckLink drivers (optional, for Blackmagic Design capture cards)
- GPU drivers with Mesa support (for headless OpenGL)

## Quick Start

### Coolify Deployment (Recommended)

This Dockerfile is designed for Coolify deployment. Simply:

1. **Build the image:**
   ```bash
   ./build.sh
   # or manually:
   docker build -t casparcg-server .
   ```

2. **Deploy to Coolify** with the following configuration:
   - **Ports**: 5250, 5251, 5252
   - **Environment**: Headless OpenGL with NVIDIA GPU support
   - **Privileged Mode**: Enable for GPU access
   - **GPU Access**: Enable if available on host

### Using Docker directly

1. **Build the image:**
   ```bash
   docker build -t casparcg-server .
   ```

2. **Run headless with GPU support:**
   ```bash
   docker run --gpus all --privileged \
     --device /dev/dri:/dev/dri \
     -p 5250:5250 -p 5251:5251 -p 5252:5252 \
     casparcg-server
   ```

3. **Run with DeckLink support:**
   ```bash
   docker run --privileged \
     --device /dev/dri:/dev/dri \
     --device /dev/blackmagic:/dev/blackmagic \
     -v /usr/lib/libDeckLinkAPI.so:/usr/lib/libDeckLinkAPI.so:ro \
     -p 5250:5250 -p 5251:5251 -p 5252:5252 \
     casparcg-server
   ```

4. **Run with X11 display (for debugging):**
   ```bash
   docker run -v /tmp/.X11-unix:/tmp/.X11-unix \
     -e DISPLAY=$DISPLAY \
     -p 5250:5250 -p 5251:5251 -p 5252:5252 \
     casparcg-server
   ```

5. **Run with software rendering:**
   ```bash
   docker run -e LIBGL_ALWAYS_SOFTWARE=1 \
     -p 5250:5250 -p 5251:5251 -p 5252:5252 \
     casparcg-server
   ```

## Build Options

The Dockerfile supports several build arguments that can be customized:

- `ENABLE_HTML`: Enable HTML module with CEF (default: ON)
- `USE_STATIC_BOOST`: Use static Boost libraries (default: OFF)
- `USE_SYSTEM_CEF`: Use system CEF instead of bundled (default: ON)
- `ENABLE_AVX2`: Enable AVX2 instructions (default: ON)
- `BUILD_TYPE`: Build type (default: Release)
- `PARALLEL_JOBS`: Number of parallel build jobs (default: 4)

Example with custom options:
```bash
docker build \
  --build-arg ENABLE_HTML=OFF \
  --build-arg USE_STATIC_BOOST=ON \
  --build-arg BUILD_TYPE=Debug \
  -t casparcg-server .
```

## Headless Operation

CasparCG Server is designed to run headless without requiring a display server. The OpenGL mixer and image processing work using EGL (OpenGL ES) in headless mode.

### Key Features

- **EGL Context**: Uses `EGL_PLATFORM=surfaceless` for headless OpenGL
- **No X11 Required**: Set `MESA_EGL_NO_X11=1` for pure headless operation
- **GPU Acceleration**: Works with Mesa drivers and hardware acceleration
- **DeckLink Support**: Full support for Blackmagic Design capture cards

### Environment Variables

- `EGL_PLATFORM=surfaceless` - Use headless EGL platform
- `MESA_EGL_NO_X11=1` - Disable X11 dependencies
- `LIBGL_ALWAYS_INDIRECT=0` - Use direct rendering

## GPU Support

### NVIDIA GPU

For NVIDIA GPU acceleration, ensure you have:
1. NVIDIA drivers installed on the host
2. NVIDIA Docker runtime installed
3. Run with `--gpus all` flag

### Intel/AMD GPU

For Intel/AMD GPU support:
1. Install Mesa drivers on the host
2. Mount `/dev/dri` device
3. Use `--privileged` flag

### Software Rendering

If no GPU is available or for testing:
1. Set `LIBGL_ALWAYS_SOFTWARE=1`
2. No special device mounting required

## DeckLink Support

CasparCG Server includes full support for Blackmagic Design DeckLink capture cards through the DeckLink SDK.

### Requirements

1. **Host System**: Install DeckLink drivers on the host system
2. **Device Access**: Mount `/dev/blackmagic` devices
3. **Library**: Mount the DeckLink API library

### Setup

```bash
# Check if DeckLink devices are available
ls /dev/blackmagic/

# Check if DeckLink API library exists
ls /usr/lib/libDeckLinkAPI.so

# Run with DeckLink support
docker run --privileged \
  --device /dev/blackmagic:/dev/blackmagic \
  -v /usr/lib/libDeckLinkAPI.so:/usr/lib/libDeckLinkAPI.so:ro \
  casparcg-server
```

### Configuration

Add DeckLink consumers/producers to your `casparcg.config`:

```xml
<consumers>
  <decklink>
    <device>1</device>
    <keyer>external</keyer>
    <buffer-depth>3</buffer-depth>
  </decklink>
</consumers>
```

## Volume Mounts

The container supports several volume mounts:

- `./config:/opt/casparcg/config:ro` - Configuration files (read-only)
- `./media:/opt/casparcg/media:rw` - Media files (read-write)
- `/dev/dri:/dev/dri:rw` - GPU device access
- `/usr/lib/libDeckLinkAPI.so:/usr/lib/libDeckLinkAPI.so:ro` - DeckLink API library
- `/dev/blackmagic:/dev/blackmagic` - DeckLink device access

## Environment Variables

- `DISPLAY` - X11 display (e.g., `:0`)
- `LIBGL_ALWAYS_SOFTWARE` - Force software rendering (0/1)
- `MESA_GL_VERSION_OVERRIDE` - Override OpenGL version
- `MESA_GLSL_VERSION_OVERRIDE` - Override GLSL version

## Troubleshooting

### OpenGL Issues

1. **Check GPU access:**
   ```bash
   docker exec casparcg-server glxinfo | grep "OpenGL version"
   ```

2. **Verify Mesa drivers:**
   ```bash
   docker exec casparcg-server dpkg -l | grep mesa
   ```

3. **Check X11 forwarding:**
   ```bash
   docker exec casparcg-server xdpyinfo
   ```

### Performance Issues

1. **Enable GPU acceleration:**
   - Use `--gpus all` flag
   - Mount `/dev/dri` device
   - Ensure proper drivers on host

2. **Software rendering fallback:**
   - Set `LIBGL_ALWAYS_SOFTWARE=1`
   - Accept reduced performance

### Build Issues

1. **Memory issues during build:**
   - Increase Docker memory limit
   - Reduce `PARALLEL_JOBS` build arg

2. **Dependency download failures:**
   - Check network connectivity
   - Verify CMake download mirrors

## Security Considerations

- The container runs as non-root user `casparcg`
- GPU access requires `--privileged` flag (use with caution)
- Consider using specific device mounts instead of full privileged mode
- Review volume mount permissions

## Coolify Deployment

This Dockerfile is optimized for Coolify deployment:

### Required Configuration

- **Ports**: 5250, 5251, 5252 (AMCP communication)
- **Environment**: Headless OpenGL with EGL support
- **GPU Access**: Enable if available on host system
- **Privileged Mode**: Required for GPU and device access

### Coolify Settings

1. **Port Mapping**:
   - 5250 → 5250 (Primary AMCP)
   - 5251 → 5251 (Secondary AMCP)
   - 5252 → 5252 (Tertiary AMCP)

2. **Environment Variables**:
   - `EGL_PLATFORM=surfaceless`
   - `MESA_EGL_NO_X11=1`
   - `LIBGL_ALWAYS_INDIRECT=0`

3. **Resource Allocation**:
   - **Memory**: 4GB+ recommended
   - **CPU**: 2+ cores recommended
   - **GPU**: Enable if available

4. **Volume Mounts**:
   - `./config:/opt/casparcg/config:ro`
   - `./media:/opt/casparcg/media:rw`

## Production Deployment

For production use:

1. **Use specific GPU allocation:**
   ```bash
   docker run --gpus '"device=0"' casparcg-server
   ```

2. **Limit resource usage:**
   ```bash
   docker run --memory=4g --cpus=2 casparcg-server
   ```

3. **Use health checks:**
   ```bash
   docker run --health-cmd="pgrep casparcg" casparcg-server
   ```

4. **Persistent storage:**
   ```bash
   docker run -v casparcg-data:/opt/casparcg/data casparcg-server
   ```

## Support

For issues related to:
- Docker setup: Check Docker documentation
- OpenGL/GPU: Verify host system GPU support
- CasparCG: Check main project documentation
- Build failures: Review build logs and dependencies
