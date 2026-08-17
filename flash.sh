#!/bin/bash

# =================================================================
# Xiaozhi ESP32 Conversational Client Flashing Script
# Target: ESP32-S3 (Woody Board)
# =================================================================

# 1. Locate and export ESP-IDF environment
IDF_PATHS=(
    "/home/zibet/.espressif/v6.0.1/esp-idf/export.sh"
    "$HOME/esp/esp-idf/export.sh"
    "/opt/esp-idf/export.sh"
    "$HOME/esp-idf/export.sh"
)

IDF_EXPORTED=false
for path in "${IDF_PATHS[@]}"; do
    if [ -f "$path" ]; then
        . "$path" > /dev/null 2>&1
        if command -v idf.py &> /dev/null; then
            IDF_EXPORTED=true
            break
        fi
    fi
done

if [ "$IDF_EXPORTED" = false ]; then
    echo "Error: Could not find or export ESP-IDF environment."
    exit 1
fi

# 2. Detect connected serial ports (ttyUSB or ttyACM)
echo "Searching for serial ports..."
PORTS=($(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null))

if [ ${#PORTS[@]} -eq 0 ]; then
    echo "Error: No /dev/ttyUSB* or /dev/ttyACM* ports found. Please connect your ESP32-S3 board."
    exit 1
fi

SELECTED_PORT=""

if [ ${#PORTS[@]} -eq 1 ]; then
    SELECTED_PORT=${PORTS[0]}
    echo "Only one port found: $SELECTED_PORT. Using it."
else
    echo "Multiple ports detected:"
    for i in "${!PORTS[@]}"; do
        echo "[$i] ${PORTS[$i]}"
    done
    
    read -p "Select a port index (0-$((${#PORTS[@]} - 1))) [Default: 0]: " INDEX
    
    if [ -z "$INDEX" ]; then
        INDEX=0
    fi
    
    if [[ "$INDEX" =~ ^[0-9]+$ ]] && [ "$INDEX" -lt "${#PORTS[@]}" ]; then
        SELECTED_PORT=${PORTS[$INDEX]}
    else
        echo "Invalid selection. Defaulting to ${PORTS[0]}"
        SELECTED_PORT=${PORTS[0]}
    fi
fi

# 3. Flash and monitor
echo "========================================"
echo "Flashing firmware to: $SELECTED_PORT"
echo "Flashing at 1500000bps, Monitoring at 115200bps"
echo "========================================"

# We flash the bootloader, partition table, app binary, and assets partition.
# idf.py flash will automatically flash all configured binaries in the build directory.
if [ "$1" == "--only" ]; then
    idf.py -p "$SELECTED_PORT" -b 1500000 flash
else
    idf.py -p "$SELECTED_PORT" -b 1500000 flash monitor --monitor-baud 115200
fi
