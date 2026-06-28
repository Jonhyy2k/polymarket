#!/bin/bash
# Start the LP risk dashboard (run from /home/ubuntu/polymarket/)
# Access: http://<EC2_PUBLIC_IP>:8080  (requires port 8080 open in EC2 security group)
# NOTE: public IP rotates on stop/start unless an Elastic IP is attached.
#       Find current IP: curl -s https://checkip.amazonaws.com
set -e
cd "$(dirname "$0")/.."

PID_FILE=/tmp/dashboard.pid

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    echo "Dashboard already running (PID=$(cat "$PID_FILE"))"
    exit 0
fi

echo "Starting Polymarket LP Dashboard on 0.0.0.0:8080 ..."
nohup python3 -m uvicorn dashboard.server:app \
    --host 0.0.0.0 \
    --port 8080 \
    --log-level warning \
    > /tmp/dashboard.log 2>&1 &

echo $! > "$PID_FILE"
sleep 2

if kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    echo "Dashboard running (PID=$(cat "$PID_FILE"))"
    echo "URL: http://$(curl -s --connect-timeout 3 https://checkip.amazonaws.com 2>/dev/null || echo '<public-ip>'):8080"
else
    echo "Dashboard failed to start. Check /tmp/dashboard.log"
    exit 1
fi
