# Multi-stage build for CasparCG Server
FROM docker.io/buildpack-deps:jammy AS build-deps

# Install build dependencies
RUN apt-get update && apt-get install -yq --no-install-recommends \
    autoconf \
    automake \
    cmake \
    ninja-build \
    curl \
    bzip2 \
    clang \
    g++ \
    gcc \
    git \
    gperf \
    libtool \
    make \
    perl \
    pkg-config \
    python3 \
    zlib1g-dev \
    libexpat1-dev \
    lsb-release \
    libglew-dev \
    libtbb-dev \
    libopenal-dev \
    libxcursor-dev \
    libxinerama-dev \
    libxi-dev \
    libsfml-dev \
    libxrandr-dev \
    libudev-dev \
    libglu1-mesa-dev \
    libgl1-mesa-dev \
    libegl1-mesa-dev \
    libboost-all-dev \
    libnss3-dev \
    libcups2-dev \
    libxdamage-dev \
    libxcomposite-dev \
    libatk1.0-dev \
    libatspi2.0-dev \
    libatk-bridge2.0-dev \
    libavcodec-dev \
    libavformat-dev \
    libavdevice-dev \
    libavutil-dev \
    libavfilter-dev \
    libswscale-dev \
    libpostproc-dev \
    libswresample-dev \
    nlohmann-json3-dev \
    libsimde-dev \
    && rm -rf /var/lib/apt/lists/*

# Build stage
FROM build-deps AS build

# Set build arguments with defaults
ARG ENABLE_HTML=ON
ARG USE_STATIC_BOOST=OFF
ARG USE_SYSTEM_CEF=ON
ARG ENABLE_AVX2=ON
ARG BUILD_TYPE=Release
ARG PARALLEL_JOBS=4

# Set environment variables
ENV CMAKE_BUILD_TYPE=${BUILD_TYPE}
ENV CC=clang
ENV CXX=clang++

# Create build directories
WORKDIR /build
RUN mkdir -p /build /install

# Copy CMake configuration files first for better layer caching
COPY src/CMakeLists.txt src/
COPY src/CMakeModules/ src/CMakeModules/
COPY src/version.tmpl src/

# Copy source code
COPY src/ src/

# Configure and build
RUN cmake -GNinja /build/src \
    -DENABLE_HTML=${ENABLE_HTML} \
    -DUSE_STATIC_BOOST=${USE_STATIC_BOOST} \
    -DUSE_SYSTEM_CEF=${USE_SYSTEM_CEF} \
    -DENABLE_AVX2=${ENABLE_AVX2} \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DCMAKE_INSTALL_PREFIX=/install

RUN cmake --build . --parallel ${PARALLEL_JOBS}

RUN cmake --install .

# Copy dependencies
RUN mkdir -p /install/lib && \
    /build/src/shell/copy_deps.sh /install/bin/casparcg /install/lib

# Runtime stage
FROM ubuntu:22.04 AS runtime

# Install runtime dependencies including OpenGL support
RUN apt-get update && apt-get install -yq --no-install-recommends \
    libc++1 \
    libnss3 \
    fontconfig \
    libavcodec-ffmpeg58 \
    libavformat-ffmpeg58 \
    libavutil-ffmpeg56 \
    libswscale-ffmpeg6 \
    libswresample-ffmpeg4 \
    libavfilter-ffmpeg8 \
    libavdevice-ffmpeg59 \
    libpostproc-ffmpeg55 \
    libboost-system1.74.0 \
    libboost-thread1.74.0 \
    libboost-filesystem1.74.0 \
    libboost-log1.74.0 \
    libboost-locale1.74.0 \
    libboost-regex1.74.0 \
    libboost-date-time1.74.0 \
    libboost-coroutine1.74.0 \
    libglew2.2 \
    libtbb12 \
    libopenal1 \
    libsfml-graphics2.5 \
    libsfml-window2.5 \
    # OpenGL and graphics libraries (including EGL for headless operation)
    libgl1-mesa-glx \
    libgl1-mesa-dri \
    libglu1-mesa \
    libegl1-mesa \
    libgles2-mesa \
    libvulkan1 \
    libdrm2 \
    libgbm1 \
    # X11 and display libraries (for optional display support)
    libx11-6 \
    libxrandr2 \
    libxinerama1 \
    libxi6 \
    libxcursor1 \
    libxdamage1 \
    libxcomposite1 \
    libatk1.0-0 \
    libatspi2.0-0 \
    libatk-bridge2.0-0 \
    libxcb1 \
    libxcb-dri2-0 \
    libxcb-dri3-0 \
    libxcb-present0 \
    libxcb-sync1 \
    libxshmfence1 \
    libxcb-glx0 \
    libxcb-keysyms1 \
    libxcb-image0 \
    libxcb-shm0 \
    libxcb-util1 \
    libxcb-render0 \
    libxcb-render-util0 \
    libxcb-icccm4 \
    libxcb-shape0 \
    libxcb-randr0 \
    libxcb-xfixes0 \
    libxcb-xinerama0 \
    libxcb-xkb1 \
    libxkbcommon0 \
    libxkbcommon-x11-0 \
    # Additional graphics and audio libraries
    libxss1 \
    libasound2 \
    libpulse0 \
    # Mesa OpenGL drivers
    mesa-utils \
    mesa-va-drivers \
    mesa-vdpau-drivers \
    # DeckLink support
    libudev1 \
    udev \
    && rm -rf /var/lib/apt/lists/*

# Copy CasparCG Server from build stage
COPY --from=build /install /opt/casparcg

# Copy fonts
COPY src/shell/liberation-fonts/ /opt/casparcg/fonts/

# Set working directory
WORKDIR /opt/casparcg

# Create non-root user for security
RUN useradd -m -u 1000 casparcg && \
    chown -R casparcg:casparcg /opt/casparcg

# Switch to non-root user
USER casparcg

# Set environment variables
ENV LD_LIBRARY_PATH=/opt/casparcg/lib
ENV DISPLAY=:0
ENV LIBGL_ALWAYS_SOFTWARE=0
ENV MESA_GL_VERSION_OVERRIDE=4.5
ENV MESA_GLSL_VERSION_OVERRIDE=450
# Headless OpenGL support
ENV EGL_PLATFORM=surfaceless
ENV MESA_EGL_NO_X11=1
ENV LIBGL_ALWAYS_INDIRECT=0

# Expose CasparCG AMCP ports
EXPOSE 5250
EXPOSE 5251
EXPOSE 5252

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD pgrep casparcg || exit 1

# Default command
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
