#!/usr/bin/env python3
import platform

from shared.docker.game_entrypoint import main

# The image builds for the machine it runs on, and each architecture keeps a
# build directory and a package name of its own.
is_arm64 = platform.machine() in ("aarch64", "arm64")
main(platform="linux-arm64" if is_arm64 else "linux")
