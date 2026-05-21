#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-cyberrt:ubuntu24-dev}"
BASE_IMAGE="${BASE_IMAGE:-ubuntu:24.04}"
CONTAINER_NAME="${CONTAINER_NAME:-cyberrt_ubuntu24_dev_${USER}}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="/workspace/CyberRT-10.0.0"

usage() {
  cat <<EOF_USAGE
Usage: $(basename "$0") <build|start|run|shell|exec|stop|restart|status>

Commands:
  build    Build the Ubuntu 24.04 CyberRT development image.
  start    Start the shared development container in the background.
  run      Start if needed, then enter the shared container.
  shell    Alias for run.
  exec     Enter the existing shared container.
  stop     Stop and remove the shared container.
  restart  Stop, then start the shared container.
  status   Show the shared container status.

Environment:
  IMAGE_NAME      Docker image name. Default: cyberrt:ubuntu24-dev
  BASE_IMAGE      Base image for build. Default: ubuntu:24.04
  CONTAINER_NAME  Shared container name. Default: cyberrt_ubuntu24_dev_\$USER
EOF_USAGE
}

container_exists() {
  docker ps -a --format '{{.Names}}' | grep -Fxq "${CONTAINER_NAME}"
}

container_running() {
  docker ps --format '{{.Names}}' | grep -Fxq "${CONTAINER_NAME}"
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

start_container() {
  if container_running; then
    echo "Container ${CONTAINER_NAME} is already running."
    return
  fi

  if container_exists; then
    docker start "${CONTAINER_NAME}" >/dev/null
    echo "Started existing container ${CONTAINER_NAME}."
    return
  fi

  docker run -d \
    --name "${CONTAINER_NAME}" \
    --network host \
    -v "${REPO_ROOT}:${WORKDIR}" \
    -w "${WORKDIR}" \
    "${IMAGE_NAME}" \
    sleep infinity >/dev/null
  echo "Started new container ${CONTAINER_NAME}."
}

exec_shell() {
  if ! container_running; then
    echo "Container ${CONTAINER_NAME} is not running. Start it with:"
    echo "  $0 start"
    exit 1
  fi
  docker exec -it "${CONTAINER_NAME}" /bin/bash
}

run_shell() {
  start_container
  exec_shell
}

stop_container() {
  if container_exists; then
    docker rm -f "${CONTAINER_NAME}" >/dev/null
    echo "Removed container ${CONTAINER_NAME}."
  else
    echo "Container ${CONTAINER_NAME} does not exist."
  fi
}

status_container() {
  if container_exists; then
    docker ps -a --filter "name=^/${CONTAINER_NAME}$"
  else
    echo "Container ${CONTAINER_NAME} does not exist."
  fi
}

case "${1:-}" in
  build)
    build_image
    ;;
  start)
    start_container
    ;;
  run|shell)
    run_shell
    ;;
  exec)
    exec_shell
    ;;
  stop)
    stop_container
    ;;
  restart)
    stop_container
    start_container
    ;;
  status)
    status_container
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
