#!/usr/bin/env bash
# Healthcheck script for TON validator-engine
# Returns 0 if healthy, 1 if unhealthy

# Check if validator-engine process is running
if ! pgrep -f "validator-engine" > /dev/null; then
    exit 1
fi

# Check if config.json exists (indicates initialization completed)
if [ ! -f "/var/ton-work/db/config.json" ]; then
    exit 1
fi

# Try to connect to console port if available
CONSOLE_PORT=${CONSOLE_PORT:-30002}
if command -v nc > /dev/null 2>&1; then
    if ! nc -z localhost $CONSOLE_PORT 2>/dev/null; then
        # Port check failed, but process is running - might be starting up
        # This is not necessarily unhealthy, so we'll be lenient
        exit 0
    fi
fi

exit 0

