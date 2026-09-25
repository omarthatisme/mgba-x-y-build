#!/usr/bin/env bash
# collect_runtime_dlls.sh
#
# Ensures a staged Windows package directory is a self-contained,
# runnable mGBA distribution by discovering and copying the full
# transitive closure of runtime DLL dependencies for every .exe/.dll
# already present in the staging tree.
#
# This exists because upstream mGBA's tools/deploy-win.sh filters
# dependencies with `grep -i mingw`, which only matches a classic
# /mingw64 MSYS2 layout. This workflow builds with MSYS2 UCRT64,
# where runtime DLLs resolve under /ucrt64/..., so that filter finds
# nothing and compiler/runtime DLLs (libwinpthread-1.dll,
# libstdc++-6.dll, libgcc_s_seh-1.dll) and non-Qt runtime deps such as
# FFmpeg's swscale-10.dll are silently dropped from the package.
#
# Usage: collect_runtime_dlls.sh <stage_dir> <msys2_prefix>
#   stage_dir     Directory tree containing the extracted CPack package
#                 (mGBA.exe, libmgba*.dll, Qt DLLs/plugins, etc).
#   msys2_prefix  MSYS2 install prefix whose DLLs are vendorable,
#                 e.g. /ucrt64. Dependencies resolving outside this
#                 prefix (Windows system DLLs) are never copied.
set -euo pipefail

STAGE_DIR="${1:?usage: collect_runtime_dlls.sh <stage_dir> <msys2_prefix>}"
MSYS_PREFIX="${2:?usage: collect_runtime_dlls.sh <stage_dir> <msys2_prefix>}"
MSYS_PREFIX="${MSYS_PREFIX%/}"

if [ ! -d "$STAGE_DIR" ]; then
    echo "collect_runtime_dlls: staging directory '$STAGE_DIR' does not exist" >&2
    exit 1
fi

if ! command -v ntldd >/dev/null 2>&1 && ! command -v objdump >/dev/null 2>&1; then
    echo "collect_runtime_dlls: neither ntldd nor objdump is available" >&2
    exit 1
fi

list_staged_binaries() {
    find "$STAGE_DIR" -type f ( -iname '*.exe' -o -iname '*.dll' )
}

# Print one resolved dependency path per line for a given binary.
# Prefers ntldd -R (recursive resolver that reports full paths).
# Falls back to reading the raw DLL import names via objdump and
# resolving each one against PATH (which is set to the MSYS2 env).
resolve_deps() {
    local bin="$1"
    if command -v ntldd >/dev/null 2>&1; then
        ntldd -R "$bin" 2>/dev/null \
            | grep -v 'not found' \
            | awk -F'=>' 'NF>1{print $2}' \
            | sed -E 's/^[[:space:]]+//; s/[[:space:]]*(0x[0-9a-fA-F]+)[[:space:]]*$//' \
            | tr -d '\r'
        return
    fi
    local name
    objdump -p "$bin" 2>/dev/null | awk '/DLL Name:/{print $3}' | while IFS= read -r name; do
        command -v "$name" 2>/dev/null || true
    done
}

# List dependency names (not paths) that ntldd/gdb could NOT resolve,
# for the final verification pass.
unresolved_deps() {
    local bin="$1"
    if command -v ntldd >/dev/null 2>&1; then
        ntldd -R "$bin" 2>/dev/null | grep -i 'not found' | awk '{print $1}' || true
    fi
}

declare -A SEEN_BIN
declare -A HAVE_DLL

# Seed HAVE_DLL with everything already staged, so we never fetch a
# duplicate of a DLL that's already present (possibly via windeployqt).
while IFS= read -r existing; do
    HAVE_DLL["$(basename "$existing" | tr '[:upper:]' '[:lower:]')"]=1
done < <(list_staged_binaries)

added_any=1
pass=0
while [ "$added_any" -eq 1 ]; do
    added_any=0
    pass=$((pass + 1))
    echo "collect_runtime_dlls: dependency scan pass $pass"

    while IFS= read -r bin; do
        [ -n "${SEEN_BIN[$bin]:-}" ] && continue
        SEEN_BIN[$bin]=1

        while IFS= read -r dep; do
            [ -z "$dep" ] && continue
            # Normalize any backslashes from ntldd/Windows-style output.
            dep="${dep//\\//}"
            case "$dep" in
                "$MSYS_PREFIX"/*) : ;;   # vendorable MSYS2 dependency
                *) continue ;;           # Windows system DLL: skip
            esac
            [ -f "$dep" ] || continue

            base_lc="$(basename "$dep" | tr '[:upper:]' '[:lower:]')"
            if [ -n "${HAVE_DLL[$base_lc]:-}" ]; then
                continue
            fi

            destdir="$(dirname "$bin")"
            cp -uv "$dep" "$destdir/"
            HAVE_DLL[$base_lc]=1
            added_any=1
        done < <(resolve_deps "$bin")
    done < <(list_staged_binaries)
done

echo "collect_runtime_dlls: dependency closure complete after $pass pass(es)"

# Final verification: fail loudly if anything staged still references
# an MSYS2/runtime DLL that never got resolved. (Genuine Windows API
# sets like KERNEL32/USER32/api-ms-win-* are expected to show as
# resolvable via the Windows loader path and are not flagged here;
# only names ntldd/gdb explicitly could not find at all are checked.)
missing_found=0
while IFS= read -r bin; do
    while IFS= read -r missing_name; do
        [ -z "$missing_name" ] && continue
        echo "collect_runtime_dlls: ERROR: $bin is missing dependency: $missing_name" >&2
        missing_found=1
    done < <(unresolved_deps "$bin")
done < <(list_staged_binaries)

if [ "$missing_found" -ne 0 ]; then
    echo "collect_runtime_dlls: one or more binaries have unresolved runtime dependencies" >&2
    exit 1
fi

echo "collect_runtime_dlls: package appears self-contained"
