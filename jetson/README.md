# Jetson Orin Nano setup

This records the working setup on the Jetson Orin Nano Super running WendyOS 0.20.2, L4T R39.2, and CUDA 13.2.

## Ethernet and GameBoy

The Mac shares its internet connection over Ethernet. The Mac was at `192.168.2.1` and the Jetson at `192.168.2.2`. Verify the address before using it on a fresh setup.

```sh
wendy --device 192.168.2.2:50051 device shell
```

The working hotspot uses the stock `rtl8822ce` module (`rtl88x2ce` driver) on `wlan0`, SSID `GameBoy`, and 2.4 GHz channel 1. The Jetson is `10.42.0.1/24`. The Chromatic was leased `10.42.0.60`. Use the current lease when testing another client.

The saved `GameBoy-Live` NetworkManager profile has autoconnect disabled and the DHCP unit is transient. On the Jetson, bring them up after a reboot with:

```sh
nmcli connection up GameBoy-Live ifname wlan0
systemd-run --unit=wendy-gameboy-live-dhcp /usr/bin/dnsmasq \
  --keep-in-foreground --port=0 --interface=wlan0 --bind-interfaces \
  --except-interface=lo --dhcp-range=10.42.0.20,10.42.0.100,255.255.255.0,2h \
  --dhcp-option=3 --dhcp-option=6 --dhcp-leasefile=/run/wendy-gameboy-live.leases
iw dev wlan0 info
cat /run/wendy-gameboy-live.leases
```

If the saved profile is missing, recreate it and enter the WPA2 password at the prompt. On a fresh image, check that no other hotspot profile or DHCP service is using `wlan0` first.

```sh
nmcli connection add type wifi ifname wlan0 con-name GameBoy-Live ssid GameBoy \
  connection.autoconnect no 802-11-wireless.mode ap 802-11-wireless.band bg \
  802-11-wireless.channel 1 wifi-sec.key-mgmt wpa-psk \
  ipv4.method manual ipv4.addresses 10.42.0.1/24 ipv4.never-default yes \
  ipv6.method disabled
nmcli --ask connection up GameBoy-Live ifname wlan0
```

DHCP supplies local addresses without advertising a default gateway or DNS. Ethernet supplies internet to the Jetson. Concurrent Wi-Fi uplink with the experimental `rtw88` driver stalled the Jetson during NetworkManager activation, even with 5 GHz disabled. Keep the stock driver for this setup.

## Docker and GPU

The tested installation used the [official Docker Engine 29.9.3 ARM64 static archive](https://download.docker.com/linux/static/stable/aarch64/docker-29.9.3.tgz). Extract its `docker/` contents into `/opt/docker-engine/` on the Jetson. Symlink `docker` and `dockerd` from that directory into `/usr/local/bin/`. Copy the checked-in files as follows:

| File | Jetson destination |
| --- | --- |
| [daemon.json](docker/daemon.json) | `/etc/docker/daemon.json` |
| [docker.service](docker/docker.service) | `/etc/systemd/system/docker.service` |
| [wendy-crun](docker/wendy-crun) | `/opt/docker-engine/wendy-crun` (mode `0755`) |

The unit orders Docker after WendyOS's existing `containerd.service`. The runtime wrapper uses the installed `crun` and `jq` to remove empty OCI disk I/O settings on this kernel, which lacks `io.max`. Docker uses host networking for workloads that need access to GameBoy or the Ethernet uplink. The default Docker bridge and automatic firewall rules are disabled.

```sh
dockerd --validate --config-file=/etc/docker/daemon.json
systemctl daemon-reload
systemctl enable --now docker.service
docker run --rm --network none hello-world
docker run --rm --network host alpine:3.23 ping -c 3 10.42.0.60
nvidia-ctk cdi list
```

Docker startup is enabled in systemd. It was verified live and has yet to be tested across a reboot. The live CUDA test verified six CPU cores with no CPU quota and a cuBLAS matrix multiply on the GPU. For GPU inference, use a Jetson-compatible ARM64 image with its inference libraries:

```sh
docker run --rm --network host --device nvidia.com/gpu=all YOUR_IMAGE
```

## LFM2.5 benchmark

The [Liquid AI LFM2.5-230M](https://huggingface.co/LiquidAI/LFM2.5-230M-GGUF) Q4_K_M model was tested in 25 W mode with llama.cpp. The [SM87 CUDA build](https://github.com/hybridgroup/llama-cpp-builder/releases/tag/b11530) ran inside Docker using GPU offload and Flash Attention. The CPU run used [llama.cpp b12359](https://github.com/ggml-org/llama.cpp/releases/tag/b12359).

| Backend | Prompt processing, 256 tokens | Generation, 128 tokens |
| --- | ---: | ---: |
| Orin GPU | 7,500 tok/s | 159.3 tok/s |
| CPU, 6 threads | 416 tok/s | 98.6 tok/s |

GPU parameters were `llama-bench -m <model> -p 256 -n 128 -t 4 -ngl 99 -fa 1 -r 5`. CPU parameters used `-t 6 -ngl 0 -r 4`. The GPU benchmark's 256-token prompt processing time was about 34 ms with the model already loaded. End-to-end time to first token was not measured. The weights and benchmark binaries were deleted afterward.
