#!/bin/bash
# RunPod start-up for Spirula Studio: system packages, the Vulkan fix, and an
# up-to-date build of the remote-monitor branch. Safe to run on every start:
# packages are re-installed (the container disk is wiped on a restart), the
# source and build live on /workspace and only rebuild what changed.
#
#   bash /workspace/runpod_setup.sh            # set up + build
#   SKIP_BUILD=1 bash /workspace/runpod_setup.sh   # packages + checks only
set -euo pipefail

WS=/workspace
# Source and build on the container disk: the global volume (S3/geesefs) cannot
# chmod, which breaks git and executables. Rebuilt from scratch on each start.
SRC=/root/spirula-studio
BRANCH=remote-monitor
REPO=https://github.com/D1odeKing/spirula-studio.git
BUILD_ARGS=(-DSS_BACKEND=cuda -DSS_BUILD_SFM=ON -DSS_BUILD_SAM=ON -DSS_BUILD_GUI=ON)

say()  { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
say "System packages"
# ---------------------------------------------------------------------------
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# libx11-6 / libxext6 / libegl1 / libgl1 / libglvnd0: what libGLX_nvidia.so needs to
# load ("vkCreateInstance: Found no drivers" without them). X11/Wayland -dev: the GUI's
# GLFW; xvfb + Mesa GL: what `spirula gui --remote` draws with (docs/notes/remote-gui.md).
apt-get install -y -qq --no-install-recommends \
    libx11-6 libxext6 libegl1 libgl1 libglvnd0 \
    vulkan-tools libvulkan-dev \
    git cmake ninja-build build-essential pkg-config \
    ffmpeg tmux rsync htop wget ca-certificates python3 openssl \
    xorg-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libgl-dev \
    xvfb libgl1-mesa-dri libglx-mesa0 >/dev/null

# ---------------------------------------------------------------------------
say "GPU, CUDA and Vulkan"
# ---------------------------------------------------------------------------
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader \
    || die "nvidia-smi failed: no GPU visible in this pod"

[ "${NVIDIA_DRIVER_CAPABILITIES:-}" = "all" ] || \
    echo "WARNING: NVIDIA_DRIVER_CAPABILITIES='${NVIDIA_DRIVER_CAPABILITIES:-}' -- set it to 'all' in the template if Vulkan fails"

if ! command -v nvcc >/dev/null; then
    for d in /usr/local/cuda/bin /usr/local/cuda-*/bin; do
        [ -x "$d/nvcc" ] && export PATH="$d:$PATH" && break
    done
fi
command -v nvcc >/dev/null || die "no nvcc: use a CUDA *-devel* image (e.g. nvidia/cuda:12.x-devel-ubuntu22.04)"
nvcc --version | tail -1

VK=$(vulkaninfo --summary 2>/dev/null) || die "Vulkan does not start (vulkaninfo --summary); SfM cannot run on this pod"
echo "$VK" | grep -q "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU" \
    || die "Vulkan sees no NVIDIA GPU (only a software device?); SfM cannot run on this pod"
echo "$VK" | grep -E "deviceName|driverInfo" | head -2

# The three features the SfM module refuses to start without.
FEAT=$(vulkaninfo 2>/dev/null)
for f in shaderFloat64 shaderBufferFloat32AtomicAdd shaderIntegerDotProduct; do
    echo "$FEAT" | grep -E "^\s*$f\s*=\s*true" >/dev/null \
        && echo "  $f: yes" || die "Vulkan device lacks $f, which spirula sfm needs"
done

free -g | awk '/Mem:/ {print "RAM: " $2 " GB total, " $7 " GB available"}'
df -h "$WS" | awk 'NR==2 {print "/workspace: " $4 " free"}'
df -h /    | awk 'NR==2 {print "container disk: " $4 " free (fast; use it for extracted frames)"}'

# ---------------------------------------------------------------------------
say "Source: $BRANCH"
# ---------------------------------------------------------------------------
if [ -d "$SRC/.git" ]; then
    git -C "$SRC" fetch -q origin "$BRANCH"
    if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]; then
        echo "local changes in $SRC -- not updating; build as it is"
    else
        git -C "$SRC" checkout -q "$BRANCH"
        git -C "$SRC" merge -q --ff-only "origin/$BRANCH" || echo "cannot fast-forward; building the current checkout"
    fi
else
    git clone -q -b "$BRANCH" "$REPO" "$SRC"
fi
git -C "$SRC" log --oneline -1

# ---------------------------------------------------------------------------
if [ "${SKIP_BUILD:-0}" != "1" ]; then
    say "Build (incremental after the first time)"
    cd "$SRC"
    # build_develop.bash sizes -j from /proc/meminfo and nproc, which in a container are the
    # host's: a 62 GB pod on a big host ran out of memory. Pin it to the pod's own limits,
    # about 5 GB per nvcc job, through the cores nproc sees.
    MEM_GB=$(( $(cat /sys/fs/cgroup/memory.max 2>/dev/null | grep -E '^[0-9]+$' || echo 68719476736) / 1073741824 ))
    # cgroup v2 (cpu.max) or v1 (cpu.cfs_quota_us); nproc alone reports the host's cores.
    { read -r QUOTA PERIOD < /sys/fs/cgroup/cpu.max; } 2>/dev/null || {
        QUOTA=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null || echo -1)
        PERIOD=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us 2>/dev/null || echo 100000)
        [ "$QUOTA" -gt 0 ] 2>/dev/null || QUOTA=max; }
    CORES=$(nproc); [ "${QUOTA:-max}" != max ] && CORES=$(( QUOTA / PERIOD ))
    JOBS=$(( MEM_GB / 5 )); [ "$JOBS" -gt "$CORES" ] && JOBS=$CORES; [ "$JOBS" -lt 1 ] && JOBS=1
    echo "pod limits: ${MEM_GB} GB, ${CORES} cores -> ${JOBS} build jobs"
    taskset -c 0-$(( JOBS - 1 )) bash build_develop.bash "${BUILD_ARGS[@]}"
    say "Self-test"
    ./build_cuda/sfm_monitor_test | tail -1
    ./build_cuda/remote_gui_test | tail -1
    # Accepted by the owner; the record lives in ~/.config, wiped on restart.
    ./build_cuda/spirula --accept-license sam3=yes --accept-license sam2=yes --accept-license gdino=yes --help >/dev/null 2>&1 || true
fi

# Model cache on the volume (plain data files, so geesefs is fine), downloaded once.
mkdir -p "$WS/models" ~/.cache/spirula-studio
[ -L ~/.cache/spirula-studio/models ] || { rm -rf ~/.cache/spirula-studio/models; ln -s "$WS/models" ~/.cache/spirula-studio/models; }

# A shell that finds the binary without typing the build path.
grep -q 'spirula-studio/build_cuda' ~/.bashrc 2>/dev/null || \
    echo "export PATH=$SRC/build_cuda:\$PATH" >> ~/.bashrc
export PATH="$SRC/build_cuda:$PATH"

# ---------------------------------------------------------------------------
say "Ready"
# ---------------------------------------------------------------------------
cat <<EOF
spirula: $(command -v spirula || echo "$SRC/build_cuda/spirula")

Work in tmux (a dropped SSH session must not kill a run):
  tmux new -s run

Frames out of the .insv files (one folder per lens), onto the fast local disk:
  spirula sam extract --help

SfM, watched from the laptop:
  spirula sfm auto /data/images -o /data/ws --manifest /workspace/manifest.yaml --monitor 7008

Training:
  spirula train --data /data/ws --cap-max 20000000 --train-resolution-divisor 2 \\
      --viewer-host 127.0.0.1 --viewer-token \$(openssl rand -hex 16) --keep-viewer-alive 0

The whole GUI, in a browser on the laptop (paths and settings are this pod's):
  tmux new -d -s gui 'spirula gui --remote 7010 2>&1 | tee /root/gui.log'
  grep -o 'http://[^ ]*' /root/gui.log      # the address, once it has started

On the laptop (RunPod's "SSH over exposed TCP" port and host):
  ssh -N -L 7010:127.0.0.1:7010 -L 7008:127.0.0.1:7008 -L 7007:127.0.0.1:7007 root@<ip> -p <port>
EOF
