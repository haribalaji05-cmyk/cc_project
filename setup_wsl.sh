#!/bin/bash
# setup_wsl.sh - One-time WSL setup and quick-start for Mini-UnionFS

set -e

echo "=== Mini-UnionFS WSL Setup ==="

# ── 1. Enable FUSE in WSL ────────────────────────────────────────────────────
echo ""
echo "[1/4] Enabling FUSE kernel module..."
sudo modprobe fuse 2>/dev/null || true

# Check /dev/fuse exists
if [ ! -c /dev/fuse ]; then
    echo "ERROR: /dev/fuse not found."
    echo "Your WSL kernel may not support FUSE."
    echo "Try: wsl --update  (in Windows PowerShell)"
    echo "Or add to /etc/wsl.conf:  [boot] systemd=true"
    exit 1
fi
echo "  /dev/fuse found. OK."

# ── 2. Install packages ──────────────────────────────────────────────────────
echo ""
echo "[2/4] Installing dependencies..."
sudo apt-get update -q
sudo apt-get install -y fuse3 libfuse3-dev pkg-config gcc make

# ── 3. Build ─────────────────────────────────────────────────────────────────
echo ""
echo "[3/4] Building mini_unionfs..."
make clean && make

# ── 4. Quick smoke test ──────────────────────────────────────────────────────
echo ""
echo "[4/4] Running quick smoke test..."

mkdir -p ./lower ./upper ./mnt
echo "hello from lower" > ./lower/hello.txt
echo "secret in lower"  > ./lower/secret.txt

./mini_unionfs ./lower ./upper ./mnt -s &
FUSE_PID=$!
sleep 1

echo "  Files in mount:"
ls ./mnt/

echo "  Reading hello.txt via mount:"
cat ./mnt/hello.txt

echo "  Modifying hello.txt (CoW trigger)..."
echo "modified" >> ./mnt/hello.txt

echo "  Upper dir now contains:"
ls ./upper/

echo "  Lower hello.txt unchanged:"
cat ./lower/hello.txt

echo "  Deleting secret.txt..."
rm ./mnt/secret.txt
echo "  Whiteout created: $(ls ./upper/)"

echo ""
echo "Cleaning up..."
kill $FUSE_PID 2>/dev/null
sleep 0.5
fusermount3 -u ./mnt 2>/dev/null || umount ./mnt 2>/dev/null || true
rm -rf ./lower ./upper ./mnt

echo ""
echo "=== Setup complete! ==="
echo ""
echo "Usage:"
echo "  mkdir -p lower upper mnt"
echo "  ./mini_unionfs ./lower ./upper ./mnt -f   # foreground (Ctrl+C to stop)"
echo "  ./mini_unionfs ./lower ./upper ./mnt -s   # background"
echo "  fusermount3 -u ./mnt                      # unmount"
echo "  ./test_unionfs.sh                         # run full test suite"
