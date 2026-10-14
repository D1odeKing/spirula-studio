# Spirula Studio for RunPod, prebuilt: a pod is ready when the image is pulled.
#
#   docker build -t <registry>/<you>/splat-runpod:<tag> tools/runpod/   (no GPU needed)
#   docker push  <registry>/<you>/splat-runpod:<tag>
#
# Spirula   /opt/spirula-studio/build_cuda/spirula   on PATH as `spirula`
# Data, models and outputs go on the /workspace volume; nothing is built at pod start.
# The GUI is built too: SPIRULA_GUI_PORT=7010 in the template serves it to a browser
# over an SSH tunnel (start.sh, docs/notes/remote-gui.md).
FROM nvidia/cuda:12.8.1-devel-ubuntu24.04

# graphics: without it the container gets CUDA but no Vulkan, and spirula sfm cannot start.
ENV NVIDIA_DRIVER_CAPABILITIES=all \
    DEBIAN_FRONTEND=noninteractive

# libx11-6 / libxext6 / libegl1 / libgl1 / libglvnd0: what libGLX_nvidia.so needs to load
# (the fix for "vkCreateInstance: Found no drivers"). The -dev and gcc-14 lines are
# LichtFeld's Linux prerequisites (docs/building_and_distribution.md); the X11/Wayland
# -dev packages also build Spirula's GUI (GLFW). xvfb and Mesa's GL are what the GUI
# draws its interface with on a pod, which has no display.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libx11-6 libxext6 libegl1 libgl1 libglvnd0 vulkan-tools libvulkan1 libvulkan-dev \
        git curl wget unzip zip tar ca-certificates pkg-config ninja-build build-essential ccache \
        gcc-14 g++-14 python3 python3-dev python3-pip nasm autoconf autoconf-archive automake libtool \
        libxinerama-dev libxcursor-dev xorg-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev \
        libegl-dev libdecor-0-dev libibus-1.0-dev libdbus-1-dev libsystemd-dev libgtk-3-dev \
        ffmpeg tmux rsync htop openssh-server openssl \
        libwayland-bin wayland-protocols libgl-dev \
        xvfb libgl1-mesa-dri libglx-mesa0 \
    && rm -rf /var/lib/apt/lists/*

# CMake 4 (LichtFeld needs 3.30+; Ubuntu 24.04 ships 3.28).
ARG CMAKE_VERSION=4.0.3
RUN wget -q https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh \
    && sh cmake-${CMAKE_VERSION}-linux-x86_64.sh --skip-license --prefix=/usr/local \
    && rm cmake-${CMAKE_VERSION}-linux-x86_64.sh

# Parallel compile jobs. nvcc jobs take several GB each: size this to the build machine's
# memory (about 5 GB a job), not its core count, or the build runs out of memory.
ARG BUILD_JOBS=8

# --- Spirula Studio: a fat binary for every GPU we rent (no GPU while the image builds).
# 8.0 A100, 8.6 A6000/RTX 30xx, 8.9 L40S/L4/RTX 40xx/RTX 6000 Ada, 9.0 H100, 12.0 RTX 50xx/PRO Blackwell
ARG SPIRULA_REPO=https://github.com/D1odeKing/spirula-studio.git
ARG SPIRULA_REF=remote-monitor
ARG SPIRULA_CUDA_ARCHS="8.0 8.6 8.9 9.0 12.0"
RUN git clone -b ${SPIRULA_REF} ${SPIRULA_REPO} /opt/spirula-studio \
    && cd /opt/spirula-studio \
    && cmake -G Ninja -B build_cuda -DCMAKE_BUILD_TYPE=Release -DSS_BACKEND=cuda \
           -DSS_BUILD_SFM=ON -DSS_BUILD_SAM=ON -DSS_BUILD_GUI=ON \
           "-DTORCH_CUDA_ARCH_LIST=${SPIRULA_CUDA_ARCHS}" \
    && cmake --build build_cuda -j ${BUILD_JOBS} \
    && ./build_cuda/sfm_monitor_test | tail -1 \
    && ./build_cuda/remote_gui_test | tail -1 \
    && find build_cuda -name '*.o' -delete \
    && ln -s /opt/spirula-studio/build_cuda/spirula /usr/local/bin/spirula

# JupyterLab for RunPod's Connect -> Jupyter button (start.sh runs it when JUPYTER_PASSWORD is set).
RUN pip3 install --no-cache-dir --break-system-packages jupyterlab

COPY start.sh /start.sh
RUN chmod +x /start.sh
WORKDIR /workspace
CMD ["/start.sh"]
