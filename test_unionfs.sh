#!/bin/bash
# test_unionfs.sh - Automated test suite for Mini-UnionFS
# Tests all core requirements + extra features

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "${GREEN}PASSED${NC}"; PASS=$((PASS + 1)); }
fail() { echo -e "${RED}FAILED${NC} — $1"; FAIL=$((FAIL + 1)); }

section() { echo -e "\n${BLUE}━━━ $1 ━━━${NC}"; }

cleanup() {
    fusermount3 -u "$MOUNT_DIR" 2>/dev/null || \
    fusermount -u "$MOUNT_DIR" 2>/dev/null || \
    umount "$MOUNT_DIR" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}

# ── Setup ────────────────────────────────────────────────────────────────────

echo -e "${YELLOW}Mini-UnionFS Test Suite${NC}"
echo "Binary: $FUSE_BINARY"
echo "Test env: $TEST_DIR"

if [ ! -f "$FUSE_BINARY" ]; then
    echo -e "${RED}ERROR: Binary '$FUSE_BINARY' not found. Run 'make' first.${NC}"
    exit 1
fi

cleanup
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

# Populate lower layer
echo "base_only_content"              > "$LOWER_DIR/base.txt"
echo "to_be_deleted"                  > "$LOWER_DIR/delete_me.txt"
echo "lower_version"                  > "$LOWER_DIR/override.txt"
echo "upper_version"                  > "$UPPER_DIR/override.txt"
mkdir -p "$LOWER_DIR/subdir"
echo "nested_file"                    > "$LOWER_DIR/subdir/nested.txt"
echo "lower_cow_target"               > "$LOWER_DIR/cow_test.txt"

# Mount (background, single-threaded)
"$FUSE_BINARY" "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR" -s &
FUSE_PID=$!

# Poll until mount is ready (up to 5 s) — necessary on slow WSL kernels
MOUNTED=0
for i in $(seq 1 10); do
    sleep 0.5
    if grep -q "$MOUNT_DIR" /proc/mounts 2>/dev/null || mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        MOUNTED=1; break
    fi
done
if [ "$MOUNTED" -eq 0 ]; then
    echo -e "${RED}ERROR: Mount did not appear after 5s. Check FUSE is enabled in WSL.${NC}"
    kill $FUSE_PID 2>/dev/null
    rm -rf "$TEST_DIR"
    exit 1
fi
echo "Mount OK"

# ── Tests ────────────────────────────────────────────────────────────────────

section "Core: Layer Visibility"

echo -n "Test 1a: Lower-layer file visible... "
grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null && pass || fail "base.txt not readable"

echo -n "Test 1b: Upper overrides lower for same filename... "
grep -q "upper_version" "$MOUNT_DIR/override.txt" 2>/dev/null && pass || fail "override.txt did not show upper version"

echo -n "Test 1c: Subdirectory from lower is visible... "
[ -d "$MOUNT_DIR/subdir" ] && pass || fail "subdir not visible"

echo -n "Test 1d: Nested file in lower subdir visible... "
grep -q "nested_file" "$MOUNT_DIR/subdir/nested.txt" 2>/dev/null && pass || fail "nested.txt not readable"

section "Core: Copy-on-Write"

echo -n "Test 2a: Append to lower-layer file triggers CoW... "
echo "appended_content" >> "$MOUNT_DIR/cow_test.txt" 2>/dev/null
sleep 0.2
UPPER_HAS=$(grep -c "appended_content" "$UPPER_DIR/cow_test.txt" 2>/dev/null)
LOWER_HAS=$(grep -c "appended_content" "$LOWER_DIR/cow_test.txt" 2>/dev/null)
if [ "$UPPER_HAS" -ge 1 ] && [ "$LOWER_HAS" -eq 0 ]; then pass
else fail "upper=$UPPER_HAS lower=$LOWER_HAS"; fi

echo -n "Test 2b: Lower file is unmodified after CoW... "
grep -q "lower_cow_target" "$LOWER_DIR/cow_test.txt" 2>/dev/null && pass || fail "lower was modified"

echo -n "Test 2c: CoW file still readable via mount... "
grep -q "appended_content" "$MOUNT_DIR/cow_test.txt" 2>/dev/null && pass || fail "appended content not readable via mount"

section "Core: Whiteout (Deletion)"

echo -n "Test 3a: Deleting lower-layer file hides it in mount... "
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null
sleep 0.2
[ ! -f "$MOUNT_DIR/delete_me.txt" ] && pass || fail "file still visible after deletion"

echo -n "Test 3b: Whiteout marker created in upper... "
[ -f "$UPPER_DIR/.wh.delete_me.txt" ] && pass || fail ".wh.delete_me.txt not found in upper"

echo -n "Test 3c: Original lower file untouched... "
[ -f "$LOWER_DIR/delete_me.txt" ] && pass || fail "lower file was physically deleted"

section "Extra: Create New Files"

echo -n "Test 4a: Create new file in mount goes to upper... "
echo "brand_new" > "$MOUNT_DIR/newfile.txt" 2>/dev/null
sleep 0.2
[ -f "$UPPER_DIR/newfile.txt" ] && grep -q "brand_new" "$UPPER_DIR/newfile.txt" && pass || fail "newfile.txt not in upper"

echo -n "Test 4b: New file readable via mount... "
grep -q "brand_new" "$MOUNT_DIR/newfile.txt" 2>/dev/null && pass || fail "newfile.txt not readable via mount"

section "Extra: Directory Operations"

echo -n "Test 5a: Create new directory in mount... "
mkdir "$MOUNT_DIR/new_dir" 2>/dev/null
sleep 0.2
[ -d "$UPPER_DIR/new_dir" ] && pass || fail "new_dir not in upper"

echo -n "Test 5b: New directory visible in mount listing... "
[ -d "$MOUNT_DIR/new_dir" ] && pass || fail "new_dir not visible in mount"

echo -n "Test 5c: Create file inside new directory... "
echo "in_new_dir" > "$MOUNT_DIR/new_dir/inside.txt" 2>/dev/null
sleep 0.2
[ -f "$UPPER_DIR/new_dir/inside.txt" ] && pass || fail "inside.txt not in upper/new_dir"

section "Extra: Rename"

echo -n "Test 6a: Rename upper file... "
echo "rename_test" > "$MOUNT_DIR/old_name.txt" 2>/dev/null
sleep 0.1
mv "$MOUNT_DIR/old_name.txt" "$MOUNT_DIR/new_name.txt" 2>/dev/null
sleep 0.2
[ -f "$MOUNT_DIR/new_name.txt" ] && [ ! -f "$MOUNT_DIR/old_name.txt" ] && pass || fail "rename failed"

section "Extra: Stat / getattr"

echo -n "Test 7a: stat() works on lower file... "
stat "$MOUNT_DIR/base.txt" &>/dev/null && pass || fail "stat failed"

echo -n "Test 7b: stat() returns ENOENT for whiteout'd file... "
stat "$MOUNT_DIR/delete_me.txt" &>/dev/null && fail "should return ENOENT" || pass

section "Extra: Truncate"

echo -n "Test 8a: Truncate lower-layer file triggers CoW... "
echo "truncate_me_content" > "$LOWER_DIR/trunc_test.txt"
truncate -s 5 "$MOUNT_DIR/trunc_test.txt" 2>/dev/null
sleep 0.2
[ -f "$UPPER_DIR/trunc_test.txt" ] && pass || fail "CoW not triggered for truncate"

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
TOTAL=$((PASS + FAIL))
echo -e "Results: ${GREEN}$PASS passed${NC} / ${RED}$FAIL failed${NC} / $TOTAL total"

if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
else
    echo -e "${RED}Some tests failed. Check the log: $UPPER_DIR/.unionfs.log${NC}"
fi

# ── Teardown ─────────────────────────────────────────────────────────────────

echo ""
echo "Unmounting..."
kill $FUSE_PID 2>/dev/null
sleep 0.5
fusermount3 -u "$MOUNT_DIR" 2>/dev/null || \
fusermount -u "$MOUNT_DIR" 2>/dev/null || \
umount "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$TEST_DIR"
echo "Done."
exit $FAIL
