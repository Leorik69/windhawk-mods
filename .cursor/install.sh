#!/usr/bin/env bash
# Idempotent environment bootstrap for Windhawk mod development.
#
# Installs the llvm-mingw cross toolchain (clang++ targeting
# i686/x86_64/aarch64-w64-mingw32) used to compile-check mods on Linux, matching
# the toolchain Windhawk uses on Windows. The Windhawk SDK headers ship in the
# repo under .vscode/windhawk_headers_*, so nothing else is required.
set -euo pipefail

# Pinned llvm-mingw release. Bump both together with a matching asset.
LLVM_MINGW_VERSION="20260908"
LLVM_MINGW_CRT="ucrt"          # UCRT matches the Windows 10+ target Windhawk uses.
LLVM_MINGW_HOST="ubuntu-22.04-x86_64"
LLVM_MINGW_ASSET="llvm-mingw-${LLVM_MINGW_VERSION}-${LLVM_MINGW_CRT}-${LLVM_MINGW_HOST}"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_ASSET}.tar.xz"

INSTALL_ROOT="${HOME}/.local/share"
INSTALL_DIR="${INSTALL_ROOT}/llvm-mingw"
VERSION_STAMP="${INSTALL_DIR}/.windhawk-version"

probe_cc="${INSTALL_DIR}/bin/x86_64-w64-mingw32-clang++"

if [[ -x "${probe_cc}" && -f "${VERSION_STAMP}" && "$(cat "${VERSION_STAMP}" 2>/dev/null)" == "${LLVM_MINGW_VERSION}" ]]; then
    echo "llvm-mingw ${LLVM_MINGW_VERSION} already installed at ${INSTALL_DIR}"
else
    echo "Installing llvm-mingw ${LLVM_MINGW_VERSION} to ${INSTALL_DIR}"
    mkdir -p "${INSTALL_ROOT}"
    tmp_tar="$(mktemp --suffix=.tar.xz)"
    trap 'rm -f "${tmp_tar}"' EXIT

    curl -fL --retry 3 --retry-delay 2 -o "${tmp_tar}" "${LLVM_MINGW_URL}"

    rm -rf "${INSTALL_DIR}" "${INSTALL_ROOT}/${LLVM_MINGW_ASSET}"
    tar -xf "${tmp_tar}" -C "${INSTALL_ROOT}"
    mv "${INSTALL_ROOT}/${LLVM_MINGW_ASSET}" "${INSTALL_DIR}"
    echo "${LLVM_MINGW_VERSION}" > "${VERSION_STAMP}"
    rm -f "${tmp_tar}"
    trap - EXIT
fi

# Make the toolchain available on the PATH for interactive shells. The mod
# checker also finds it at this default location without any PATH changes.
PROFILE_LINE='export PATH="$HOME/.local/share/llvm-mingw/bin:$PATH"'
PROFILE_FILE="${HOME}/.bashrc"
if [[ -f "${PROFILE_FILE}" ]] && ! grep -qF "${PROFILE_LINE}" "${PROFILE_FILE}"; then
    printf '\n# Windhawk mod toolchain\n%s\n' "${PROFILE_LINE}" >> "${PROFILE_FILE}"
fi

export PATH="${INSTALL_DIR}/bin:${PATH}"
echo "Toolchain versions:"
x86_64-w64-mingw32-clang++ --version | head -n 1

# Sanity check: the checker must be able to find the toolchain and headers.
python3 "$(dirname "$0")/../scripts/check_mod.py" --help >/dev/null
echo "Environment ready. Run: python3 scripts/check_mod.py"
