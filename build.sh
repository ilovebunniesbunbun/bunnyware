#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if [[ -t 1 ]]; then
    RED=$'\033[1;31m' GREEN=$'\033[1;32m' YELLOW=$'\033[1;33m' BLUE=$'\033[1;34m' CYAN=$'\033[1;36m' DIM=$'\033[2m' BOLD=$'\033[1m' RESET=$'\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' CYAN='' DIM='' BOLD='' RESET=''
fi

BUILD_DIR="build"
BUILD_TYPE="Release"
WITH_TESTS=1
RUN_TESTS=0
CLEAN=0
START=0
START_ARGS=()

function info() { echo "${BLUE}==>${RESET} ${BOLD}$*${RESET}"; }
function ok()   { echo "${GREEN}✔${RESET} $*"; }
function warn() { echo "${YELLOW}!${RESET} $*"; }
function die()  { echo "${RED}✖${RESET} $*" >&2; exit 1; }

function usage() {
    cat <<USAGE
${BOLD}bunnyware build script${RESET}

${CYAN}Usage:${RESET} ./build.sh [options] [-- <bunnyware args>]

${CYAN}Options:${RESET}
  ${GREEN}-h, --help${RESET}       Show this help and exit
  ${GREEN}-s, --start${RESET}      Start bunnyware after a successful build (uses sudo unless already root)
  ${GREEN}-t, --test${RESET}       Run the unit tests after building
  ${GREEN}-c, --clean${RESET}      Remove the build directory before configuring
  ${GREEN}-d, --debug${RESET}      Build with CMAKE_BUILD_TYPE=Debug (default: Release)
  ${GREEN}--no-tests${RESET}       Configure without the unit test target

${CYAN}Examples:${RESET}
  ${DIM}./build.sh${RESET}                     Build with tests enabled
  ${DIM}./build.sh -t${RESET}                  Build and run the unit tests
  ${DIM}./build.sh -s${RESET}                  Build, then start the overlay
  ${DIM}./build.sh -s -- --x11${RESET}         Build, then start with the X11 backend
  ${DIM}./build.sh -c -d -s${RESET}            Clean debug build, then start
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -s|--start) START=1 ;;
        -t|--test) RUN_TESTS=1 ;;
        -c|--clean) CLEAN=1 ;;
        -d|--debug) BUILD_TYPE="Debug" ;;
        --no-tests) WITH_TESTS=0 ;;
        --) shift; START_ARGS=("$@"); break ;;
        *) usage >&2; echo >&2; die "Unknown option: $1" ;;
    esac
    shift
done

if [[ $RUN_TESTS -eq 1 && $WITH_TESTS -eq 0 ]]; then
    die "--test cannot be combined with --no-tests"
fi

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    info "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

info "Configuring ($BUILD_TYPE, tests=$([[ $WITH_TESTS -eq 1 ]] && echo on || echo off))"
cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUNNYWARE_BUILD_TESTS="$([[ $WITH_TESTS -eq 1 ]] && echo ON || echo OFF)"

info "Building with $(nproc) jobs"
make -j"$(nproc)" -C "$BUILD_DIR"
ok "Build finished: ${BOLD}$BUILD_DIR/bunnyware${RESET}"

if [[ $RUN_TESTS -eq 1 ]]; then
    info "Running unit tests"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    ok "Tests passed"
fi

if [[ $START -eq 1 ]]; then
    if [[ $EUID -eq 0 ]]; then
        SUDO=()
    else
        SUDO=(sudo)
        warn "bunnyware needs root for /dev/input/event* input injection and process memory reads, invoking sudo"
    fi
    info "Starting bunnyware ${START_ARGS[*]:-}"
    exec "${SUDO[@]}" "./$BUILD_DIR/bunnyware" "${START_ARGS[@]}"
fi
