#!/bin/bash
# Run dotsim's tests in CT 123, the container with ESP-IDF, kode-sdk, aiohttp and Playwright.
# Usage (from the worktree root on pve2): dotsim/tests/run-in-ct.sh [pytest args...]
# Copies this checkout to /opt/kode/pro-test (no .git, no Rust target) and runs pytest there.
set -e
ROOT=$(git rev-parse --show-toplevel)
CT=${DOTSIM_CT:-123}
tar -C "$ROOT" --exclude=.git --exclude=target --exclude=.superpowers --exclude='dotsim/apps/*/build' -cf - . \
  | pct exec "$CT" -- bash -c 'rm -rf /opt/kode/pro-test && mkdir -p /opt/kode/pro-test && tar -xf - -C /opt/kode/pro-test'
pct exec "$CT" -- bash -c "cd /opt/kode/pro-test && ln -sfn /opt/kode/src/kode-sdk dotsim/hostsim/sdk && \
  /opt/kode/pro-venv/bin/python -m pytest $* "
