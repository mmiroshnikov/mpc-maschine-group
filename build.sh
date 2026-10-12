#!/bin/sh
# Offline test, then the arm build. Needs mpc-vst-plugins next to this repo, or MPC_VST.
cd "$(dirname "$0")"
ROOT="${MPC_VST:-../mpc-vst-plugins}"
"$ROOT/tools/test_port.sh" vst/vst.json && "$ROOT/tools/build_port.sh" vst/vst.json
