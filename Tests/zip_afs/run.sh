#!/usr/bin/env bash
# Build and run the ZIP AbstractFileSystem test (Linux, needs: g++ >= 13, zlib, python3, zip)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
CXX="${CXX:-g++}"

echo "== build"
"$CXX" -std=c++23 -O1 -g -pthread -Wall -Wfatal-errors -Wshadow -Wno-unused-function -Wno-maybe-uninitialized \
    -DWXINTL_NO_GETTEXT_MACRO -I"$ROOT" -I"$ROOT/zenXml" -include "zen/i18n.h" \
    "$ROOT/Tests/zip_afs/zip_afs_test.cpp" \
    "$ROOT/FreeFileSync/Source/afs/zip.cpp" \
    "$ROOT/FreeFileSync/Source/afs/abstract.cpp" \
    "$ROOT"/zen/{zstring,file_path,resolve_path,file_access,file_io,file_traverser,sys_error,sys_info,sys_version,thread,format_unit,legacy_compiler,process_exec}.cpp \
    -lz -o "$WORK/zip_afs_test"

echo "== fixtures"
REF="$WORK/ref"
mkdir -p "$REF/sub dir/nested" "$REF/empty folder" "$REF/Données été"
echo "hello" > "$REF/a.txt"
head -c 300000 /dev/urandom > "$REF/sub dir/random.bin"                            # incompressible
yes "compressible line" | head -n 50000 > "$REF/sub dir/nested/big.txt"            # multiple inflate blocks
: > "$REF/sub dir/empty.txt"
echo "accents" > "$REF/Données été/fichier é.txt"
touch -d "2024-02-29 13:37:42" "$REF/a.txt"

cd "$REF"
python3 - "$WORK" <<'EOF'
import os, sys, zipfile
work = sys.argv[1]
def add_all(zf, compress_type):
    for dirpath, dirnames, filenames in os.walk("."):
        for d in dirnames:
            zf.write(os.path.join(dirpath, d), compress_type=compress_type)
        for f in filenames:
            zf.write(os.path.join(dirpath, f), compress_type=compress_type)
with zipfile.ZipFile(f"{work}/py_deflate.zip", "w") as zf: add_all(zf, zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(f"{work}/py_stored.zip",  "w") as zf: add_all(zf, zipfile.ZIP_STORED)

# stored archive with corrupted payload => CRC must be detected
with zipfile.ZipFile(f"{work}/corrupt.zip", "w") as zf:
    zf.writestr("corrupt.txt", "some payload that will be modified")
data = bytearray(open(f"{work}/corrupt.zip", "rb").read())
pos = data.find(b"some payload")
data[pos] ^= 0xFF
open(f"{work}/corrupt.zip", "wb").write(data)
EOF
zip -q -r "$WORK/infozip.zip" .                 # Info-ZIP: extended time stamps, UTF-8 names
zip -q -r -fz "$WORK/infozip64.zip" .           # force Zip64 extra fields
cd "$WORK"

echo "== test"
for archive in py_deflate py_stored infozip infozip64; do
    "$WORK/zip_afs_test" "$WORK/$archive.zip" "$REF"
done
"$WORK/zip_afs_test" --expect-read-error "$WORK/corrupt.zip" corrupt.txt

echo "== all ZIP tests passed"
