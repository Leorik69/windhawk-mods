#!/usr/bin/env bash
# Idempotent setup for the Windhawk mods development environment.
#
# Prepares everything a Cloud Agent needs to author and validate a Windhawk mod
# on Linux: the C++ formatter, the Node/TypeScript deploy tooling and the Python
# validation tooling. Actual mod *compilation* requires Windhawk on Windows and
# is intentionally out of scope here (see .github/workflows/mod_compatibility_check.yml).
set -euo pipefail

cd "$(dirname "$0")/.."

echo "==> Installing clang-format (C++ mod formatter)"
if ! command -v clang-format >/dev/null 2>&1; then
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends clang-format
fi
clang-format --version

echo "==> Installing Node dependencies (deploy/catalog tooling)"
# The dev dependencies are type declarations used for type checking, so unlike
# the deploy workflow (which omits them) we install everything for `tsc`.
npm ci

echo "==> Installing Python dependencies (PR validation tooling)"
# pyyaml is what pr_validation.py imports; requirements.txt covers the rest.
python3 -m pip install --user --quiet --upgrade pyyaml -r .github/requirements.txt

echo "==> Development environment ready"
