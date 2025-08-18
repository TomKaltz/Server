# Multi-stage build for CasparCG Server
FROM docker.io/buildpack-deps:jammy AS build-casparcg

# Copy and run the existing install-dependencies script
COPY tools/linux/install-dependencies /install-dependencies
RUN apt-get update && /install-dependencies

# Create build directories
RUN mkdir /source && mkdir /build && mkdir /install

# Copy source code
COPY ./src /source

# Set working directory
WORKDIR /build

# Set build arguments with defaults (matching the working Dockerfile)
ARG CC=clang
ARG CXX=clang++
ARG GIT_HASH
ARG ENABLE_HTML=ON
ARG USE_STATIC_BOOST=ON
ARG USE_SYSTEM_CEF=OFF
ARG ENABLE_AVX2=ON
ARG BUILD_TYPE=Release

# Set environment variables
ENV CC=${CC}
ENV CXX=${CXX}

# Configure and build (using the working configuration)
RUN cmake -GNinja /source \
    -DUSE_STATIC_BOOST=${USE_STATIC_BOOST} \
    -DUSE_SYSTEM_CEF=${USE_SYSTEM_CEF} \
    -DENABLE_HTML=${ENABLE_HTML} \
    -DENABLE_AVX2=${ENABLE_AVX2} \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE}

RUN cmake --build .

RUN cmake --install . --prefix staging

# Copy dependencies (using the working approach)
RUN ln -s /build/staging /staging && \
    /source/shell/copy_deps.sh /staging/bin/casparcg /staging/lib

# Runtime stage - use NVIDIA OpenGL base for proper GPU support
FROM docker.io/nvidia/opengl:1.2-glvnd-devel-ubuntu22.04 AS runtime

# Install minimal runtime dependencies
RUN set -ex; \
    apt-get update; \
    DEBIAN_FRONTEND="noninteractive" apt-get install -y --no-install-recommends \
        tzdata \
        libc++1 \
        libnss3 \
        fontconfig \
        ; \
    rm -rf /var/lib/apt/lists/*

# Copy CasparCG Server from build stage
COPY --from=build-casparcg /staging /opt/casparcg

# Set working directory
WORKDIR /opt/casparcg

# Set environment variables for headless OpenGL
ENV LD_LIBRARY_PATH=/opt/casparcg/lib
ENV LIBGL_ALWAYS_SOFTWARE=0
ENV MESA_GL_VERSION_OVERRIDE=4.5
ENV MESA_GLSL_VERSION_OVERRIDE=450
ENV EGL_PLATFORM=surfaceless
ENV MESA_EGL_NO_X11=1
ENV LIBGL_ALWAYS_INDIRECT=0

# Expose CasparCG AMCP ports
EXPOSE 5250
EXPOSE 5251
EXPOSE 5252

# Copy the run script for headless operation
COPY tools/linux/run_docker.sh ./run_docker.sh

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD pgrep casparcg || exit 1

# Default command - use direct binary for headless operation
CMD ["./bin/casparcg"]

# Alternative: Use the run script for auto-restart functionality
# CMD ["./run.sh"]

# Build and run instructions:
# 
# Build the image:
# docker build -t casparcg-server .
#
# Run headless with GPU support (recommended for production):
# docker run --gpus all --privileged --device /dev/dri:/dev/dri \
#   -p 5250:5250 -p 5251:5251 -p 5252:5252 casparcg-server
#
# Run with DeckLink support (requires host DeckLink drivers):
# docker run --privileged --device /dev/blackmagic:/dev/blackmagic \
#   -v /usr/lib/libDeckLinkAPI.so:/usr/lib/libDeckLinkAPI.so:ro \
#   -p 5250:5250 -p 5251:5251 -p 5252:5252 casparcg-server
#
# Run with X11 display (for debugging/development):
# docker run -v /tmp/.X11-unix:/tmp/.X11-unix \
#   -e DISPLAY=$DISPLAY -p 5250:5250 -p 5251:5251 -p 5252:5252 casparcg-server
#
# Run with software rendering (fallback):
# docker run -e LIBGL_ALWAYS_SOFTWARE=1 \
#   -p 5250:5250 -p 5251:5251 -p 5252:5252 casparcg-server
#
# Note: CasparCG is designed to run headless using EGL for OpenGL rendering.
# The mixer and image processing work without a display server.
# 
# For Coolify deployment, this Dockerfile provides a production-ready
# CasparCG server with headless OpenGL support.
