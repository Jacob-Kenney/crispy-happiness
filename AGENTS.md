# Working on the Jetson

These instructions apply to Jetson application code and device changes in this repository. Keep the source and build configuration in Git. See [Jetson recovery](jetson/README.md) for the installed Docker configuration and GameBoy recovery commands.

## Connect and check the device

The development board is a Jetson Orin Nano Super running WendyOS 0.20.2, Linux ARM64, and CUDA 13.2. It was reachable over the Mac's Ethernet link at `192.168.2.2:50051`. Confirm the address and identity when switching devices or networks.

```sh
jetson=192.168.2.2:50051
wendy --device "$jetson" device info
wendy --device "$jetson" device shell -- sh -c 'uname -m; df -h /; systemctl is-active docker; iw dev wlan0 info'
```

`wendy device shell` runs as root on the host. Use it for inspection, deployment, and verification. Its command output comes from a PTY and may contain CRLF line endings and mixed stderr. Keep Ethernet available during Wi-Fi work. The stock driver runs the GameBoy access point at `10.42.0.1`. Driver swaps have caused lockups. For ordinary app work, use the existing network and boot configuration.

## Build and deploy

- Edit and test on the development machine first. Use `uv run` for Python development and tests.
- Build container images for `linux/arm64`. For CUDA code, target Orin's `sm_87` and use libraries compatible with the installed Jetson CUDA stack. The host GPU driver and CDI device alone do not supply every inference library inside an image.
- For a Wendy app, keep `wendy.json` and its build or Compose configuration together. From the app directory, use `wendy --device "$jetson" run --detach --wait-ready` when deploying. The [LFM3 Omni configuration](lfm3-omni/wendy.json) shows the app format and GPU entitlement.
- For standalone Docker tests, use `--rm` for short-lived containers. Use `--network host` when the app needs GameBoy or the Jetson's Ethernet uplink. Docker's default bridge is disabled on this device. Bind a GameBoy-facing service to `10.42.0.1` when only hotspot clients should reach it. With host networking the app uses the host's ports directly.
- For GPU workloads, request the device with `--device nvidia.com/gpu=all`. Standard containers have access to all six CPU cores unless their launch settings impose a limit.

```sh
wendy --device "$jetson" device shell -- docker run --rm \
  --network host --device nvidia.com/gpu=all YOUR_ARM64_IMAGE
```

Supply credentials at deployment time. Keep Wi-Fi passwords, app API keys, firmware dumps, and model weights out of Git.

## Verify and clean up

1. Check the container logs and exercise its real endpoint or command. For inference, run a real request and confirm GPU execution. GPU enumeration alone does not verify compute.
2. For GameBoy-facing changes, check the access point and test from a hotspot client when possible. From the Jetson, `ping -I wlan0 -c 3 CLIENT_IP` checks connectivity to an associated client. Read the current DHCP lease from `/run/wendy-gameboy-live.leases`.
3. Report the device, image or commit, and what ran. For model benchmarks, record the quantization, power mode, input/output lengths, and throughput. Measure actual time to first token from a streaming client if requested.
4. Remove temporary containers and artifacts created solely for the test. Keep recovery commands in [jetson/README.md](jetson/README.md).
