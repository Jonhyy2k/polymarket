#!/usr/bin/env bash
# EC2 Setup Script for Polymarket Arbitrage Detector
# Target: Amazon Linux 2023 / Ubuntu 22.04+ on z1d.3xlarge (eu-west-2b)
#
# Usage: ssh into EC2, then:
#   git clone <your-repo> && cd polymarket/cpp
#   chmod +x deploy/setup_ec2.sh && ./deploy/setup_ec2.sh

set -euo pipefail

echo "=== Polymarket Arb Detector — EC2 Setup ==="

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    OS="unknown"
fi

echo "[1/5] Installing dependencies ($OS)..."

if [ "$OS" = "amzn" ] || [ "$OS" = "amazon" ]; then
    # Amazon Linux 2023
    sudo dnf install -y gcc-c++ cmake openssl-devel boost-devel git
elif [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    # Ubuntu / Debian
    sudo apt-get update -qq
    sudo apt-get install -y build-essential cmake libssl-dev libboost-system-dev libboost-all-dev git
else
    echo "[WARN] Unknown OS: $OS — install gcc, cmake, openssl-dev, boost-dev manually"
fi

echo "[2/5] Downloading simdjson..."
mkdir -p deps
if [ ! -f deps/simdjson.h ] || [ ! -f deps/simdjson.cpp ]; then
    curl -sL "https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.h" -o deps/simdjson.h
    curl -sL "https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.cpp" -o deps/simdjson.cpp
    echo "  Downloaded simdjson"
else
    echo "  simdjson already present"
fi

echo "[3/5] Building (Release, -O3 -march=native)..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPM_ALLOW_CONDA_BOOST=OFF
cmake --build build -j"$(nproc)"

echo "[4/5] Creating logs directory..."
mkdir -p logs

echo "[5/5] Verifying build..."
test -x ./build/arb_detector
ldd ./build/arb_detector

echo ""
echo "=== Build complete ==="
echo ""
echo "Run in foreground:"
echo "  ./deploy/run_ec2.sh ./config.json"
echo ""
echo "Benchmark for 75s:"
echo "  ./deploy/benchmark_ec2.sh ./config.json 75"
echo ""
echo "Run in background with logging:"
echo "  nohup ./deploy/run_ec2.sh ./config.json > logs/stdout.log 2>&1 &"
echo ""
echo "Monitor:"
echo "  tail -f logs/stdout.log"
echo "  tail -f logs/arb_log.csv"
