#!/usr/bin/env bash
# Wrapper gọi manifests/ovs-setup.sh
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

sudo bash "$ROOT_DIR/manifests/ovs-setup.sh"
