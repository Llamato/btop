#!/bin/sh
# Post-reboot check for DRM_IOCTL_ASAHI_GET_STATS (Asahi AGX) + btop integration.
# Run on the machine with the patched kernel.

set -e
DIR=$(dirname "$(readlink -f "$0")")
STATS=/tmp/agxprobe/stats
LOAD="EGL_PLATFORM=surfaceless glmark2 -b offscreen:duration=12 -b build:duration=12"

if [ ! -x "$STATS" ]; then
	echo "building probe"
	gcc -O2 -o "$STATS" "$DIR/asahi_stats_probe.c"
fi

card=$(for c in /sys/class/drm/card[0-9]; do [ "$(readlink "$c/device/driver" 2>/dev/null)" = asahi ] && basename "$c"; done | head -1)

echo "== uname: $(uname -r)"
echo "== asahi card: ${card:-none}"

echo
echo "== idle sample"
"$STATS" || true

echo
echo "== sample while GPU work is in flight"
(nix-shell -p glmark2 --run "$LOAD" > /tmp/glmark2.log 2>&1 &)
sleep 4
for i in 1 2 3; do
	"$STATS" || true
	sleep 2
done
wait

echo
echo "== btop (debug log)"
if [ ! -x /tmp/btop-build/btop ]; then
	echo "building btop"
	(cd "$DIR/.." && nix-shell -p cmake gcc15 ninja --run "cmake -B /tmp/btop-build -DCMAKE_BUILD_TYPE=Debug -G Ninja -DBUILD_TESTING=OFF > /dev/null && ninja -C /tmp/btop-build -j$(nproc)") > /tmp/btop_build.log 2>&1 || echo "btop build failed, see /tmp/btop_build.log"
fi
rm -rf /tmp/btop-gputest && mkdir -p /tmp/btop-gputest/state
HOME=/tmp/btop-gputest XDG_STATE_HOME=/tmp/btop-gputest/state python3 "$DIR/ptyrun.py" 45 150 6 \
	/tmp/btop-build/btop -d > /tmp/btop_gpu_frame.raw 2>&1 || true
python3 "$DIR/ansi2screen.py" /tmp/btop_gpu_frame.raw 45 150 | grep -i -B 2 -A 8 "gpu" || true
grep -i asahi /tmp/btop-gputest/state/btop.log | tail -8
