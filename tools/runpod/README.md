# Spirula Studio on RunPod

The image and pod scripts for running Spirula Studio on a rented NVIDIA GPU
(RunPod, or any Docker host with the NVIDIA container toolkit).

| file | |
|---|---|
| `Dockerfile` | a prebuilt image: CUDA build with SfM, SAM and the GUI, Xvfb and Mesa for the remote GUI, SSH, JupyterLab |
| `start.sh` | the image's start command: SSH, model cache on `/workspace`, licences, a GPU/Vulkan check, and the remote GUI when `SPIRULA_GUI_PORT` is set |
| `runpod_setup.sh` | the same for a stock CUDA *devel* image: installs packages and builds the `remote-monitor` branch on the pod |

```bash
docker build -t <registry>/<you>/splat-runpod:<tag> tools/runpod/
docker push  <registry>/<you>/splat-runpod:<tag>
```

Template settings: `NVIDIA_DRIVER_CAPABILITIES=all` (Vulkan, which SfM needs),
TCP port 22, and for the GUI `SPIRULA_GUI_PORT=7010` (optionally
`SPIRULA_GUI_TOKEN`, so the address survives a restart). Then, on the laptop,
with the pod's "SSH over exposed TCP" host and port:

```bash
ssh -N -L 7010:127.0.0.1:7010 -L 7008:127.0.0.1:7008 -L 7007:127.0.0.1:7007 root@<ip> -p <port>
ssh root@<ip> -p <port> cat /root/gui-url.txt      # open this address in a browser
```

7010 is the GUI (`docs/notes/remote-gui.md`), 7008 an SfM run's monitor and
7007 the training viewer (`docs/notes/remote-monitoring.md`). Use `127.0.0.1`,
not `localhost`: some pods resolve `localhost` to IPv6 while the servers
listen on IPv4.
