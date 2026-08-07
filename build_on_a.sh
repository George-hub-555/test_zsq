#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
FALCON_DIR=${1:-"$PROJECT_ROOT/falcon"}
OUTPUT_TGZ=${2:-"$PROJECT_ROOT/zsq_arm64_bundle.tar.gz"}

if [[ $(uname -m) != "aarch64" && $(uname -m) != "arm64" ]]; then
    echo "ERROR: A must be an ARM64 Linux machine; current architecture: $(uname -m)" >&2
    exit 1
fi
if [[ ! -f "$FALCON_DIR/WORKSPACE" && ! -f "$FALCON_DIR/WORKSPACE.bazel" ]]; then
    echo "ERROR: Falcon workspace not found: $FALCON_DIR" >&2
    exit 1
fi
BAZEL="$FALCON_DIR/devel/builder/bazel-7.4.1-linux-arm64"
if [[ ! -x "$BAZEL" ]]; then
    echo "ERROR: Bazel is missing or not executable: $BAZEL" >&2
    exit 1
fi
if [[ -e "$OUTPUT_TGZ" || -e "$OUTPUT_TGZ.sha256" ]]; then
    echo "ERROR: output already exists; choose a new output path: $OUTPUT_TGZ" >&2
    exit 1
fi

echo "[1/4] Build //test_zsq:zsq_benchmark"
cd "$FALCON_DIR"
"$BAZEL" build \
    --config=linux_arm64 \
    -c opt \
    --package_path="%workspace%:%workspace%/.." \
    //test_zsq:zsq_benchmark

BAZEL_BIN=$("$BAZEL" info bazel-bin --config=linux_arm64 --package_path="%workspace%:%workspace%/..")
BINARY="$BAZEL_BIN/test_zsq/zsq_benchmark"
if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: expected binary not found: $BINARY" >&2
    exit 1
fi
if ldd "$BINARY" | grep -q 'not found'; then
    echo "ERROR: unresolved dependencies in A build:" >&2
    ldd "$BINARY" | grep 'not found' >&2
    exit 1
fi

STAGE_DIR=$(mktemp -d)
trap 'rm -rf -- "$STAGE_DIR"' EXIT
PACKAGE_DIR="$STAGE_DIR/zsq_arm64_bundle"
mkdir -p "$PACKAGE_DIR/bin" "$PACKAGE_DIR/lib"

echo "[2/4] Collect binary and non-glibc runtime libraries"
cp -fL "$BINARY" "$PACKAGE_DIR/bin/zsq_benchmark"
chmod 755 "$PACKAGE_DIR/bin/zsq_benchmark"
GLIBC_ALLOW='^(linux-vdso|ld-linux|libc\.|libm\.|libpthread\.|libdl\.|librt\.|libresolv\.|libnsl\.|libutil\.)'
while IFS=$'\t' read -r lib path; do
    [[ -n "$lib" && -n "$path" ]] || continue
    cp -fL "$path" "$PACKAGE_DIR/lib/$lib"
    echo "  bundled: $lib <- $path"
done < <(ldd "$BINARY" | awk -v allow="$GLIBC_ALLOW" '$2 == "=>" && $1 !~ allow {print $1 "\t" $3}' | sort -u)

cp -f "$SCRIPT_DIR/run_on_b.sh" "$PACKAGE_DIR/run_on_b.sh"
cp -f "$SCRIPT_DIR/README.md" "$PACKAGE_DIR/README.md"
chmod 755 "$PACKAGE_DIR/run_on_b.sh"

echo "[3/4] Write build manifest"
{
    echo "created_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "falcon_commit=$(git -C "$FALCON_DIR" rev-parse HEAD 2>/dev/null || echo UNKNOWN)"
    echo "architecture=$(uname -m)"
    echo "kernel=$(uname -srmo)"
    echo "glibc=$(ldd --version 2>&1 | head -n 1)"
    echo "bazel=$($BAZEL --version 2>&1 | head -n 1)"
    echo "compiler=$(g++ --version 2>&1 | head -n 1)"
    echo "build_target=//test_zsq:zsq_benchmark"
    echo "build_config=--config=linux_arm64 -c opt"
    echo
    echo "[sha256]"
    (cd "$PACKAGE_DIR" && find bin lib -type f -print0 | sort -z | xargs -0 sha256sum)
    echo
    echo "[ldd-original-binary]"
    ldd "$BINARY"
} > "$PACKAGE_DIR/manifest.txt"

echo "[4/4] Create transfer archive"
mkdir -p "$(dirname -- "$OUTPUT_TGZ")"
tar -C "$STAGE_DIR" -czf "$OUTPUT_TGZ" zsq_arm64_bundle
OUTPUT_DIR=$(cd -- "$(dirname -- "$OUTPUT_TGZ")" && pwd)
OUTPUT_NAME=$(basename -- "$OUTPUT_TGZ")
(cd "$OUTPUT_DIR" && sha256sum "$OUTPUT_NAME" > "$OUTPUT_NAME.sha256")

echo "A build/package complete. Transfer these two files to B:"
echo "  $OUTPUT_TGZ"
echo "  $OUTPUT_TGZ.sha256"
