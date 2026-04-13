#!/bin/bash
# Deploy MiSTer binary to FPGA device
# Usage: ./deploy.sh [password]
PASS="${1:-1}"
HOST="root@192.168.0.7"
SRC="/home/odelot/mister/Main_MiSTer/bin/MiSTer"
DST="/tmp/MiSTer_new"

# Create SSH key pair if needed and copy to device
if [ ! -f ~/.ssh/id_rsa ]; then
    ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa
fi

echo "Deploying $SRC to $HOST..."
scp -o StrictHostKeyChecking=no "$SRC" "$HOST:$DST" && \
ssh -o StrictHostKeyChecking=no "$HOST" "killall MiSTer 2>/dev/null; cp $DST /media/fat/MiSTer; chmod +x /media/fat/MiSTer; sync; echo DEPLOYED"
