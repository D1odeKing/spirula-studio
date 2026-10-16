#!/bin/bash
# Pod start for the prebuilt image: SSH with the key RunPod passes in PUBLIC_KEY, the model
# cache and licences, a GPU/Vulkan check, the remote GUI when SPIRULA_GUI_PORT is set, then
# idle. Nothing is built here.
mkdir -p ~/.ssh /run/sshd && chmod 700 ~/.ssh
if [ -n "${PUBLIC_KEY:-}" ]; then
    echo "$PUBLIC_KEY" >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys
fi
ssh-keygen -A >/dev/null 2>&1
/usr/sbin/sshd

# Models download once, onto the volume (plain data files, so the S3-backed volume is fine).
if [ -d /workspace ]; then
    mkdir -p /workspace/models ~/.cache/spirula-studio
    [ -L ~/.cache/spirula-studio/models ] || { rm -rf ~/.cache/spirula-studio/models; ln -s /workspace/models ~/.cache/spirula-studio/models; }
fi
# Accepted by the owner; the record lives in ~/.config, which a restart wipes.
spirula --accept-license sam3=yes --accept-license sam2=yes --accept-license gdino=yes --help >/dev/null 2>&1 || true

# JupyterLab on 8888 for RunPod's HTTP proxy (Connect -> Jupyter), rooted at / so the pod's
# own disk (/root) is reachable as well as /workspace. The token is JUPYTER_PASSWORD when the
# template sets one, else a random one; the address is in /root/jupyter-url.txt. Checkpoints go
# to one folder: beside a dataset's images a .ipynb_checkpoints copy becomes one more image.
JUPYTER_TOKEN=${JUPYTER_PASSWORD:-$(openssl rand -hex 16)}
echo "http://127.0.0.1:8888/lab?token=${JUPYTER_TOKEN}" > /root/jupyter-url.txt
chmod 600 /root/jupyter-url.txt
nohup python3 -m jupyter lab --allow-root --no-browser --ip=0.0.0.0 --port=8888 \
    --IdentityProvider.token="$JUPYTER_TOKEN" --ServerApp.allow_origin='*' \
    --ServerApp.root_dir=/ --FileContentsManager.preferred_dir=workspace \
    --FileCheckpoints.checkpoint_dir=/root/.jupyter_checkpoints \
    --FileContentsManager.delete_to_trash=False > /root/jupyter.log 2>&1 &

{
    echo "== $(date)"
    nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader
    if vulkaninfo --summary 2>/dev/null | grep -q DISCRETE_GPU; then
        echo "Vulkan: OK (spirula sfm can run)"
    else
        echo "Vulkan: NOT AVAILABLE on this host -- training works, SfM does not"
    fi
    spirula --version 2>&1 | head -1
} | tee /workspace/pod-check.log 2>/dev/null

# The whole GUI in a browser (docs/notes/remote-gui.md): set SPIRULA_GUI_PORT (e.g. 7010) in
# the template, and SPIRULA_GUI_TOKEN for a token that survives restarts. Restarted when it
# exits, so quitting it from the page does not lock the page out. Address: /root/gui-url.txt.
if [ -n "${SPIRULA_GUI_PORT:-}" ]; then
    GUI_TOKEN=${SPIRULA_GUI_TOKEN:-$(openssl rand -hex 16)}
    echo "http://127.0.0.1:${SPIRULA_GUI_PORT}/?token=${GUI_TOKEN}" > /root/gui-url.txt
    chmod 600 /root/gui-url.txt
    nohup bash -c "cd /workspace 2>/dev/null; while true; do
        spirula gui --remote ${SPIRULA_GUI_PORT} --remote-token ${GUI_TOKEN}; sleep 2; done" \
        > /root/gui.log 2>&1 &
    echo "Remote GUI on port ${SPIRULA_GUI_PORT}: cat /root/gui-url.txt for the address"
fi

sleep infinity
