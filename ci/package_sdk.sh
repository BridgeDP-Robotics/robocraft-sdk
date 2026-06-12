#!/bin/bash
# =============================================================================
# SDK wrapper packaging script (data-driven)
#
# Usage: ci/package_sdk.sh <sdk_name> <arch>
#   sdk_name: booster_t1 | unitree_g1 | encos | engineai_pm01_edu | x2
#   arch:     x86_64 | aarch64
#
# Output: sdk-<sdk_name>-<arch>.tar.gz
#
# Reads so_name from sdk_wrapper/<sdk_name>/manifest.yaml and bundled_libs
# from sdk_wrapper/<sdk_name>/ci.yaml — no per-robot branches.
#
# The tarball contains the wrapper .so (at root, per sdk_diag spec),
# bundled private libs in lib/, plus a generated manifest.yaml + checksums.sha256
# produced by ci/gen_wrapper_manifest.sh.
# =============================================================================

set -euo pipefail

SDK_NAME="${1:?Usage: $0 <sdk_name> <arch>}"
ARCH="${2:?Usage: $0 <sdk_name> <arch>}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WRAPPER_DIR="${PROJECT_ROOT}/sdk_wrapper/${SDK_NAME}"
MANIFEST="${WRAPPER_DIR}/manifest.yaml"
CI_YAML="${WRAPPER_DIR}/ci.yaml"

if [ ! -d "${WRAPPER_DIR}" ]; then
    echo "ERROR: sdk_wrapper directory not found: ${WRAPPER_DIR}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Read .so name from manifest.yaml (api.entrypoint)
# ---------------------------------------------------------------------------
SO_NAME=""
if [ -f "${MANIFEST}" ]; then
    SO_NAME=$(grep -E '^\s*entrypoint:' "${MANIFEST}" | sed -E 's/.*entrypoint:\s*"?([^"]*)"?/\1/' | head -1)
fi

if [ -z "${SO_NAME}" ]; then
    echo "ERROR: Could not determine so_name from ${MANIFEST}"
    exit 1
fi

# Replace underscores with hyphens for package name (booster_t1 -> booster-t1)
SDK_NAME_HYPHEN="${SDK_NAME//_/-}"
PACKAGE_NAME="sdk-${SDK_NAME_HYPHEN}-${ARCH}"
STAGING_DIR="${PROJECT_ROOT}/${PACKAGE_NAME}"

echo "=== Packaging SDK Wrapper: ${SDK_NAME} (${ARCH}) ==="

# Clean staging directory
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}/lib"

BUILD_DIR="${PROJECT_ROOT}/build/intermediates"

# ---------------------------------------------------------------------------
# Copy wrapper .so to package root (entrypoint for sdk_diag)
# ---------------------------------------------------------------------------
cp "${BUILD_DIR}/${SO_NAME}" "${STAGING_DIR}/"
echo "  + ${SO_NAME}"

# Copy config.json if present (wrapper reads it at runtime)
if [ -f "${WRAPPER_DIR}/config.json" ]; then
    cp "${WRAPPER_DIR}/config.json" "${STAGING_DIR}/"
    echo "  + config.json"
fi

# Fix RUNPATH to $ORIGIN/lib (for bundled libs).
# Done here instead of CMake to avoid Ninja/CMake escaping issues with $.
if command -v patchelf >/dev/null 2>&1; then
    RPATH='$ORIGIN/lib'
    if [ -f "${CI_YAML}" ]; then
        CUSTOM_RPATH=$(sed -nE 's/^[[:space:]]*rpath:[[:space:]]*"([^"]*)"[[:space:]]*$/\1/p' "${CI_YAML}" | head -1)
        [ -n "${CUSTOM_RPATH}" ] && RPATH="${CUSTOM_RPATH}"
    fi
    # SDK_RPATH env var overrides ci.yaml rpath (for multi-distro builds)
    [ -n "${SDK_RPATH:-}" ] && RPATH="${SDK_RPATH}"
    patchelf --set-rpath "${RPATH}" "${STAGING_DIR}/${SO_NAME}"
    echo "  + patchelf --set-rpath ${RPATH} ${SO_NAME}"
else
    echo "WARN patchelf not found; RUNPATH may be incorrect" >&2
fi

# ---------------------------------------------------------------------------
# Copy bundled libs (from ci.yaml) if any.
# Prefers python3+PyYAML; falls back to grep/sed for minimal SDK builder
# images that lack python3.
# ---------------------------------------------------------------------------
if [ -f "${CI_YAML}" ]; then
    if python3 -c "import yaml" 2>/dev/null; then
        python3 -c "
import yaml, os, sys

with open('${CI_YAML}') as f:
    doc = yaml.safe_load(f)

libs = doc.get('bundled_libs', []) if doc else []
for lib in libs:
    src = lib['src'].replace('{arch}', '${ARCH}').replace('{build_dir}', '${BUILD_DIR}')
    dst = lib['dst']
    # Resolve relative to project root
    full_src = os.path.join('${PROJECT_ROOT}', src)
    if not os.path.exists(full_src):
        full_src = src  # absolute or build-dir-relative
    if os.path.exists(full_src):
        print(f'COPY\t{full_src}\t{dst}')
    else:
        print(f'WARN\tBundled lib not found: {src}', file=sys.stderr)
" | while IFS=$'\t' read -r action src_path dst_path; do
            case "${action}" in
                COPY)
                    cp "${src_path}" "${STAGING_DIR}/lib/${dst_path}"
                    echo "  + lib/${dst_path}"
                    ;;
            esac
        done
    else
        # Shell fallback: parse simple YAML inline-dict bundled_libs entries
        # Format: - { src: "path/{arch}/lib.so", dst: "lib.so.0" }
        while IFS= read -r line; do
            src=$(echo "$line" | sed -n 's/.*src:[[:space:]]*"\([^"]*\)".*/\1/p')
            dst=$(echo "$line" | sed -n 's/.*dst:[[:space:]]*"\([^"]*\)".*/\1/p')
            [ -z "$src" ] || [ -z "$dst" ] && continue
            src="${src//\{arch\}/${ARCH}}"
            src="${src//\{build_dir\}/${BUILD_DIR}}"
            full_src="${PROJECT_ROOT}/${src}"
            [ -f "$full_src" ] || full_src="$src"
            if [ -f "$full_src" ]; then
                cp "${full_src}" "${STAGING_DIR}/lib/${dst}"
                echo "  + lib/${dst}"
            else
                echo "WARN Bundled lib not found: ${src}" >&2
            fi
        done < <(grep -E '^\s*-.*src:.*dst:' "${CI_YAML}")
    fi
fi

# ---------------------------------------------------------------------------
# Generate manifest.yaml + checksums.sha256 (sdk_diag-ready package)
# ---------------------------------------------------------------------------
if [ -x "${SCRIPT_DIR}/gen_wrapper_manifest.sh" ]; then
    "${SCRIPT_DIR}/gen_wrapper_manifest.sh" "${STAGING_DIR}" "${MANIFEST}" "${ARCH}"
fi

# Create tarball
tar czf "${PROJECT_ROOT}/${PACKAGE_NAME}.tar.gz" -C "${STAGING_DIR}" .
rm -rf "${STAGING_DIR}"

TARBALL_SIZE=$(du -h "${PACKAGE_NAME}.tar.gz" | cut -f1)
echo ""
echo "=== SDK Package created ==="
echo "  ${PACKAGE_NAME}.tar.gz (${TARBALL_SIZE})"
