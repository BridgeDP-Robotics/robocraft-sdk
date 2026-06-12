#!/usr/bin/env bash
# =============================================================================
# ci/gen_wrapper_manifest.sh — fill in a wrapper package's manifest + checksums.
#
# Takes a committed static metadata template (sdk_wrapper/<name>/manifest.yaml)
# and emits, INTO the package directory:
#   - manifest.yaml      = template (comments stripped) + target.arch + files[]
#   - checksums.sha256   = sha256 of every payload file in the package
#
# The result satisfies the vendor manifest schema enforced by
# `sdk_diag --check` (see tools/sdk_diag).
#
# Usage: ci/gen_wrapper_manifest.sh <package_dir> <template.yaml> [arch]
#   package_dir : dir whose root holds the wrapper .so (+ lib/ private libs)
#   template    : sdk_wrapper/<wrapper>/manifest.yaml
#   arch        : x86_64 | aarch64 (default: uname -m)
# =============================================================================
set -euo pipefail

PKG="${1:?usage: $0 <package_dir> <template> [arch]}"
TPL="${2:?usage: $0 <package_dir> <template> [arch]}"
ARCH="${3:-$(uname -m)}"

[ -d "$PKG" ] || { echo "ERROR: no such package dir: $PKG" >&2; exit 1; }
[ -f "$TPL" ] || { echo "ERROR: no such template: $TPL" >&2; exit 1; }

# entrypoint .so name comes from the template (api.entrypoint).
ENTRY="$(sed -nE 's/^[[:space:]]+entrypoint:[[:space:]]*//p' "$TPL" | head -1)"
[ -n "$ENTRY" ] || { echo "ERROR: template has no api.entrypoint: $TPL" >&2; exit 1; }
[ -f "$PKG/$ENTRY" ] || { echo "ERROR: entrypoint not found in package: $PKG/$ENTRY" >&2; exit 1; }

OUT="$PKG/manifest.yaml"
CKS="$PKG/checksums.sha256"

emit_file() {  # $1 = path relative to PKG
  local rel="$1" abs="$PKG/$1" h s
  h="$(sha256sum "$abs" | awk '{print $1}')"
  s="$(stat -c %s "$abs")"
  printf '  - path: %s\n    sha256: "%s"\n    size: %s\n' "$rel" "$h" "$s" >> "$OUT"
}

# 1. static metadata (drop full-line comments; templates must avoid inline ones)
grep -vE '^[[:space:]]*#' "$TPL" > "$OUT"

# 2. target.arch (per-build, so it is injected rather than committed)
printf 'target:\n  arch: %s\n' "$ARCH" >> "$OUT"

# 3. files[] — entrypoint first, then the remaining payload files, deterministic
printf 'files:\n' >> "$OUT"
emit_file "$ENTRY"
while IFS= read -r rel; do
  [ "$rel" = "$ENTRY" ] && continue
  emit_file "$rel"
done < <(cd "$PKG" && find . -type f ! -name manifest.yaml ! -name checksums.sha256 -printf '%P\n' | sort)

# 4. checksums.sha256 over every payload file
( cd "$PKG" && find . -type f ! -name manifest.yaml ! -name checksums.sha256 -printf '%P\n' | sort \
  | while IFS= read -r p; do printf '%s  %s\n' "$(sha256sum "$p" | awk '{print $1}')" "$p"; done ) > "$CKS"

echo "  + manifest.yaml (arch=$ARCH, entrypoint=$ENTRY)"
echo "  + checksums.sha256"
