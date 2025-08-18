#!/bin/bash

# Simple build script for Coolify deployment
# Based on the working tools/linux/build-in-docker script

GIT_HASH=$(git rev-parse --verify --short HEAD 2>/dev/null || echo "unknown")

echo "Building CasparCG Server Docker image..."
echo "Git hash: $GIT_HASH"

docker build -t casparcg-server \
  --build-arg CC=clang \
  --build-arg CXX=clang++ \
  --build-arg GIT_HASH="$GIT_HASH" \
  .

if [ $? -eq 0 ]; then
    echo "✅ Build successful!"
    echo "Run with: docker run --privileged -p 5250:5250 -p 5251:5251 -p 5252:5252 casparcg-server"
else
    echo "❌ Build failed!"
    exit 1
fi
