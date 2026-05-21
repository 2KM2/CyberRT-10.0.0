#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-cyberrt:ubuntu24-dev}"
BASE_IMAGE="${BASE_IMAGE:-ubuntu:24.04}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<EOF
Usage: $(basename "$0") <build|run|shell>

Commands:
  build   Build the Ubuntu 24.04 CyberRT development image.
  run     Start an interactive shell with the repository mounted.
  shell   Alias for run.

Environment:
  IMAGE_NAME  Docker image name. Default: cyberrt:ubuntu24-dev
  BASE_IMAGE  Base image. Default: ubuntu:24.04
EOF
}

build_image() {
  docker build \
    --build-arg BASE_IMAGE="${BASE_IMAGE}" \
    --build-arg USER_UID="$(id -u)" \
    --build-arg USER_GID="$(id -g)" \
    -t "${IMAGE_NAME}" \
    -f "${REPO_ROOT}/docker/ubuntu24/Dockerfile" \
    "${REPO_ROOT}"
}

run_shell() {
  docker run --rm -it \
    --network host \
    -v "${REPO_ROOT}:/workspace/CyberRT-10.0.0" \
    -w /workspace/CyberRT-10.0.0 \
    "${IMAGE_NAME}" \
    /bin/bash
}

case "${1:-}" in
  build)
    build_image
    ;;
  run|shell)
    run_shell
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
