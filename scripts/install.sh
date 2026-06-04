#!/bin/bash
# Install Super Arp module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/fork" ]; then
    echo "Error: dist/fork not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Fork Module ==="

# Deploy to Move - midi_fx subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/midi_fx/fork"
scp -r dist/fork/* ableton@move.local:/data/UserData/schwung/modules/midi_fx/fork/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    if ls src/chain_patches/*.json 1>/dev/null 2>&1; then
        scp src/chain_patches/*.json ableton@move.local:/data/UserData/schwung/patches/
    fi
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/midi_fx/fork"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/midi_fx/fork/"
echo ""
echo "Restart Schwung to load the new module."
