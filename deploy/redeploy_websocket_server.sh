#!/bin/bash

# Default values
MEMCACHED_HOST=""
WEBSOCKET_PORT=39447
DEBUG_LEVEL="INFO"
PING_INTERVAL=20
ENVIRONMENT="TEST"
ENDPOINT_URL="http://localhost"

# Check for arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -a|--tcp-port)
            WEBSOCKET_PORT="$2"
            shift 2
            ;;
        -m|--memcached-host)
            MEMCACHED_HOST="$2"
            shift 2
            ;;
        -d|--debug-level)
            DEBUG_LEVEL="$2"
            shift 2
            ;;
        -p|--ping-interval)
            PING_INTERVAL="$2"
            shift 2
            ;;
        -e|--environment)
            ENVIRONMENT="$2"
            shift 2
            ;;
        -u|--endpoint_url)
            ENDPOINT_URL="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            exit 1
            ;;
    esac
done

echo "WEBSOCKET SERVER"

echo "MEMCACHED_HOST: $MEMCACHED_HOST"
echo "WEBSOCKET_PORT: $WEBSOCKET_PORT"
echo "DEBUG_LEVEL: $DEBUG_LEVEL"
echo "PING_INTERVAL: $PING_INTERVAL"
echo "ENVIRONMENT: $ENVIRONMENT"
echo "ENDPOINT_URL: $ENDPOINT_URL"


# Updating the Git repo
echo "Updating Git repo..."
#cd /path/to/your/git/repo
git pull

# Moving to the deployment directory
echo "Moving to deployment directory..."
cd websocket_server

# Building Docker image
echo "Building Docker image..."
docker build -t grid_detector_websocket_server -f Dockerfile .

# Stopping and removing the old container (if exists)
echo "Stopping and removing old container..."
docker stop grid_detector_websocket_server || true
docker rm grid_detector_websocket_server || true

# Deploying the new container
echo "Deploying new container..."
docker run --name grid_detector_websocket_server --restart unless-stopped --network=jaam -d  -p "$WEBSOCKET_PORT":"$WEBSOCKET_PORT" --env WEBSOCKET_PORT="$WEBSOCKET_PORT"  --env ENDPOINT_URL="$ENDPOINT_URL" --env ENVIRONMENT="$ENVIRONMENT" --env DEBUG_LEVEL="$DEBUG_LEVEL" --env PING_INTERVAL="$PING_INTERVAL" --env MEMCACHED_HOST="$MEMCACHED_HOST" grid_detector_websocket_server

echo "Container deployed successfully!"

