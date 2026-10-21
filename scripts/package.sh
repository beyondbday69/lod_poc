#!/usr/bin/env bash
#
# Builds the LOD PoC native mod and packages it as a .levipack.
#
# The layout mirrors examples/full-cpp-mod/build.ps1 from the LeviLaunchroid
# repository:
#
#   dist/arm64-v8a/lod_poc/           unpacked mod folder
#   dist/arm64-v8a/lod_poc.levipack   zip of the folder contents
#
# The package folder contains:
#
#   manifest.json
#   liblod_poc.so
#   config/config.json
#   config/config.schema.json

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

ABI="arm64-v8a"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-28}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_ROOT="${BUILD_ROOT:-${REPO_ROOT}/build}"
DIST_ROOT="${DIST_ROOT:-${REPO_ROOT}/dist}"

MOD_ID="lod_poc"
MOD_LIBRARY_NAME="lib${MOD_ID}.so"

HOST_BUILD_DIR="${BUILD_ROOT}/host"
ANDROID_BUILD_DIR="${BUILD_ROOT}/${ABI}"
DIST_ABI_DIR="${DIST_ROOT}/${ABI}"
PACKAGE_DIR="${DIST_ABI_DIR}/${MOD_ID}"
ARCHIVE_PATH="${DIST_ABI_DIR}/${MOD_ID}.levipack"

log() {
    printf '==> %s\n' "$*"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

resolve_ndk_home() {
    local candidates=()
    local root=""

    if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
        candidates+=("${ANDROID_NDK_HOME}")
    fi
    if [[ -n "${ANDROID_NDK_ROOT:-}" ]]; then
        candidates+=("${ANDROID_NDK_ROOT}")
    fi

    for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
        [[ -z "${root}" ]] && continue
        if [[ -d "${root}/ndk" ]]; then
            while IFS= read -r candidate; do
                candidates+=("${candidate}")
            done < <(find "${root}/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V -r)
        fi
        if [[ -d "${root}/ndk-bundle" ]]; then
            candidates+=("${root}/ndk-bundle")
        fi
    done

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}/build/cmake/android.toolchain.cmake" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

NDK_HOME="$(resolve_ndk_home)" || die "Android NDK not found. Set ANDROID_NDK_HOME, ANDROID_NDK_ROOT, ANDROID_HOME, or ANDROID_SDK_ROOT."
TOOLCHAIN_FILE="${NDK_HOME}/build/cmake/android.toolchain.cmake"
[[ -f "${TOOLCHAIN_FILE}" ]] || die "Android NDK toolchain not found: ${TOOLCHAIN_FILE}"

log "Android NDK: ${NDK_HOME}"

# ---------------------------------------------------------------------------
# Host build: typed config generator
# ---------------------------------------------------------------------------

log "Configuring host build"
cmake -S "${REPO_ROOT}" -B "${HOST_BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

log "Building config generator"
cmake --build "${HOST_BUILD_DIR}" --target levi_config_generator

GENERATOR="${HOST_BUILD_DIR}/levi_config_generator"
[[ -x "${GENERATOR}" ]] || die "Config generator was not produced in ${HOST_BUILD_DIR}"

# ---------------------------------------------------------------------------
# Android build
# ---------------------------------------------------------------------------

log "Configuring Android build (${ABI}, ${ANDROID_PLATFORM})"
cmake -S "${REPO_ROOT}" -B "${ANDROID_BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM="${ANDROID_PLATFORM}" \
    -DANDROID_STL="c++_shared" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

log "Building ${MOD_LIBRARY_NAME}"
cmake --build "${ANDROID_BUILD_DIR}" --target "${MOD_ID}"

LIBRARY_PATH="${ANDROID_BUILD_DIR}/out/${ABI}/${MOD_LIBRARY_NAME}"
[[ -f "${LIBRARY_PATH}" ]] || die "Built library not found: ${LIBRARY_PATH}"

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------

log "Assembling package in ${PACKAGE_DIR}"
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}/config"

cp "${REPO_ROOT}/manifest.json" "${PACKAGE_DIR}/manifest.json"
cp "${LIBRARY_PATH}" "${PACKAGE_DIR}/${MOD_LIBRARY_NAME}"

"${GENERATOR}" "${PACKAGE_DIR}/config"

# ---------------------------------------------------------------------------
# Archive
# ---------------------------------------------------------------------------

log "Creating ${ARCHIVE_PATH}"
rm -f "${ARCHIVE_PATH}"

if command -v zip >/dev/null 2>&1; then
    (cd "${PACKAGE_DIR}" && zip -r -X -9 "${ARCHIVE_PATH}" manifest.json "${MOD_LIBRARY_NAME}" config)
else
    (cd "${PACKAGE_DIR}" && python3 -m zipfile -c "${ARCHIVE_PATH}" manifest.json "${MOD_LIBRARY_NAME}" config)
fi

[[ -f "${ARCHIVE_PATH}" ]] || die "Archive was not created: ${ARCHIVE_PATH}"

log "Built LOD PoC:"
log "  ${PACKAGE_DIR}"
log "  ${ARCHIVE_PATH}"
