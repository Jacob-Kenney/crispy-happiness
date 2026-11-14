# Chromatic UDP to WebSocket bridge

This ESP32 component adds a USB serial console command to Shiny Relay 1.1.12. It bridges one configured IPv4 UDP peer and one WebSocket server. The WebSocket protocol uses one binary message with one standard gzip member per UDP datagram. It supports UDP payloads up to 1,200 bytes and reassembles fragmented WebSocket messages. Datagram boundaries are preserved, including empty datagrams. Invalid gzip and packets over the limit are dropped.

Save `GameBoy` in **Menu → System → Wi-Fi**. The firmware reconnects to this network at boot. Once you have a WebSocket and UDP peer, open the USB serial console at 115200 baud and enter your own values in this example:

```text
bridge start ws://192.168.1.20:8765/bridge 9000 192.168.1.20 9001
bridge status
bridge stop
```

The bridge sends UDP to the configured peer, which must also send from that IP and port. For a TLS WebSocket, use `wss://` with a certificate trusted by Shiny Relay's certificate bundle. `bridge start` saves the endpoint and ports in flash. The bridge restores them at boot and waits for `GameBoy` to be online. `bridge stop` clears the saved endpoint and disables bridge auto-start.

Run `bridge selftest` on the console, or use `uv run --with pyserial==3.5 chromatic-bridge/device_test.py`. The self-test uses on-device UDP and WebSocket sockets over loopback, compares binary payloads in both directions, and exercises fragmented messages and an invalid gzip checksum.

To rebuild, extract the [Shiny Relay 1.1.12 source](https://install.shinyrelay.com/download/shiny-relay-src-1.1.12.tar.gz) into `.chromatic/` and run:

```sh
sh chromatic-bridge/prepare.sh
docker run --rm --mount "type=bind,source=$PWD/.chromatic/bridge-build/mcu,target=/project" \
  -w /project espressif/idf:v5.3 idf.py \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usbcart;sdkconfig.wifiinst;sdkconfig.bridge' build
```

The resulting `mcu_fw.bin` is an ESP32 application image. The tested Shiny Relay layout has the running application at `0x10000`. Verify the installed partition table and active slot before flashing an application. The bridge uses the existing FPGA image and bootloader.
