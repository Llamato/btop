#!/bin/sh
# Post-reboot check for DRM_IOCTL_ASAHI_GET_STATS (Asahi AGX) and the btop GPU box.
# Run on the machine that has the patched asahi module loaded.
#
#   ./tools/asahi_verify.sh            # uses ~/bin/btop-asahi if it exists
#   BTOP=/path/to/btop ./tools/asahi_verify.sh

set -e
DIR=$(dirname "$(readlink -f "$0")")
STATS=/tmp/agxprobe/stats
GPULOAD=/tmp/gpuload
BTOP=${BTOP:-$HOME/bin/btop-asahi}

# The render node renumbers when the asahi module is reloaded, so find it by driver instead.
node=""
for c in /sys/class/drm/card*; do
	drv=$(readlink -f "$c/device/driver" 2>/dev/null) || continue
	[ "$drv" = asahi ] || continue
	r=$(ls "$(readlink -f "$c/device")/drm/" 2>/dev/null | grep '^renderD' | head -1) || true
	[ -z "$r" ] || node=/dev/dri/$r
done

echo "== uname: $(uname -r)"
echo "== asahi render node: ${node:-none}"
echo "== module: $(lsmod | grep '^asahi' || echo 'asahi not loaded')"

if [ ! -x "$STATS" ]; then
	echo "== building probe"
	mkdir -p /tmp/agxprobe
	gcc -O2 -o "$STATS" "$DIR/asahi_stats_probe.c"
fi

if [ ! -x "$GPULOAD" ]; then
	echo "== building gpu workload (needs EGL/GLES2 from mesa)"
	nix-shell -p gcc15 libglvnd mesa --run \
		"gcc -O2 -Wall -o $GPULOAD $DIR/asahi_gpuload.c -lEGL -lGLESv2" > /tmp/gpuload_build.log 2>&1 ||
		echo "gpuload build failed, see /tmp/gpuload_build.log"
fi

echo
echo "== idle sample"
[ -n "$node" ] && "$STATS" "$node" || true

echo
echo "== samples while GPU work is in flight"
if [ -x "$GPULOAD" ]; then
	(EGL_PLATFORM=surfaceless GALLIUM_DRIVER=asahi exec "$GPULOAD" 12 400 > /tmp/gpuload.log 2>&1) &
	loadpid=$!
	sleep 4
	for i in 1 2 3; do
		[ -n "$node" ] && "$STATS" "$node" || true
		sleep 2
	done
	wait "$loadpid" 2>/dev/null || true
else
	echo "no workload binary, idle samples only"
fi

echo
echo "== btop (debug log, 6 s in a pty)"
if [ ! -x "$BTOP" ]; then
	echo "building btop into /tmp/btop-build"
	(cd "$DIR/.." && nix-shell -p cmake gcc15 ninja --run "cmake -B /tmp/btop-build -DCMAKE_BUILD_TYPE=Debug -G Ninja -DBUILD_TESTING=OFF > /dev/null && ninja -C /tmp/btop-build -j$(nproc)") > /tmp/btop_build.log 2>&1 ||
		echo "btop build failed, see /tmp/btop_build.log"
	BTOP=/tmp/btop-build/btop
fi

rm -rf /tmp/btop-gputest && mkdir -p /tmp/btop-gputest/state
grep -q gpu0 ~/.config/btop/btop.conf 2>/dev/null || sed 's/^shown_boxes = .*/shown_boxes = "cpu mem net proc gpu0"/' \
	~/.config/btop/btop.conf > /tmp/btop-gputest/btop.conf 2>/dev/null || true

(EGL_PLATFORM=surfaceless GALLIUM_DRIVER=asahi exec "$GPULOAD" 10 400 > /dev/null 2>&1) &
loadpid=$!
HOME=/tmp/btop-gputest XDG_STATE_HOME=/tmp/btop-gputest/state timeout 20 python3 "$DIR/ptyrun.py" 50 160 8 "$BTOP" -d > /tmp/btop_gpu_frame.raw 2>&1 || true
wait "$loadpid" 2>/dev/null || true

python3 "$DIR/ansi2screen.py" /tmp/btop_gpu_frame.raw 50 160 | grep -i -B 1 -A 7 "gpu" || true
grep -i asahi /tmp/btop-gputest/state/btop.log | tail -8
