# Can2wifi

This project uses the popular ESP32 and a CAN transceiver to bridge the CAN bus to WiFi, allowing remote monitoring and control of CAN bus networks over TCP/IP. It is intended to connect PC software like [Rocrail](https://www.rocrail.online/) via WiFi to the CAN bus of a Märklin/Trix 60113 Gleisbox which comes with the Mobile Station 2. 

I used the following web sites for inspiration:
- a similar project on [https://mobatron.4lima.de/2022/05/esp8266-und-ms2](https://mobatron.4lima.de/2022/05/esp8266-und-ms2)
- the Gleisbox connector diagram on [https://www.skrauss.de/modellbahn/canbus_stecker.html](https://www.skrauss.de/modellbahn/canbus_stecker.html)
- the Märklin CAN / CAN-over-Ethernet specification [https://www.maerklin.de/fileadmin/media/produkte/CS2_can-protokoll_1-0.pdf](https://www.maerklin.de/fileadmin/media/produkte/CS2_can-protokoll_1-0.pdf)

## Features

- CAN bus connectivity using an external transceiver
- Listen on TCP port 15731 for connection requests (the implementation is limited to a single TCP client at a time)
- Forward all CAN messages to connected TCP client and vice versa
- WiFi connection to your access point (SSID and credentials in `include/secrets.h`)
- mDNS hostname: `can2wifi`
- OTA firmware update service (credentials in `include/secrets.h`)
- Web server (default port 80, [http://can2wifi.local/](http://can2wifi.local/)) for status, statistics, and CAN bus monitoring

## Hardware

- ESP32 development board (I used the devkit-v1)
- CAN transceiver (I used a SN65HVD230 CAN breakout board from Waveshare)
- Wire the `CAN_TX`, `CAN_RX`, `3V3` and `GND` pins of the ESP32 to the CAN transceiver
- Wire `CANL`, `CANH` (I also connected `GND`) of the CAN transceiver to your Gleisbox
- Some photos of my installation are found below

## Software setup

1. PlatformIO is used for build management
1. Clone the repository
1. Copy [include/secrets-template.h](include/secrets-template.h) to `include/secrets.h` and adapt `WIFI_SSID`, `WIFI_PASS` and `OTA_PASSWORD` to your setup.
1. Also configure `CAN_TX_PIN` and `CAN_RX_PIN` in `include/config.h` to match your wiring.
1. Build the firmware and upload to your ESP32 via USB
1. Monitor serial output via USB at `115200` baud for debugging
1. For subsequent updates, you can also use OTA via the Arduino IDE (select the network port) or PlatformIO:
   - Edit `platformio.ini` → `[env:esp32doit-devkit-v1_ota]` and set `upload_port` to the device IP and `--auth` in the `upload_flags` section to match your `OTA_PASSWORD`
   - Then run:
     ```bash
     platformio run -e esp32doit-devkit-v1_ota -t upload
     ```
1. Watch the status web page at [http://can2wifi.local/](http://can2wifi.local/)

## TCP ↔ CAN Forwarding

The firmware forwards CAN frames to a single TCP client and forwards TCP-framed messages to the CAN bus. It uses the protocol described in the Märklin specification referenced above.


## Example Trace

Below is an example of CAN bus traffic captured while connected via TCP:

```
TCP client connected
CAN->TCP  ID=0x00305B0E DLC=0 DATA= | P=0 ADDR=0x5B0E R=0 CMD=18 DLC=0 DATA=
CAN->TCP  ID=0x00317B41 DLC=8 DATA=47 43 86 3B 01 27 00 10 | P=0 ADDR=0x7B41 R=1 CMD=18 DLC=8 DATA=47 43 86 3B 01 27 00 10
TCP->CAN  ID=0x00319B51 DLC=8 DATA=00 00 18 01 02 01 46 FF | P=0 ADDR=0x9B51 R=1 CMD=18 DLC=8 DATA=00 00 18 01 02 01 46 FF
TCP->CAN  ID=0x00009B51 DLC=5 DATA=00 00 00 00 01 | P=0 ADDR=0x9B51 R=0 CMD=00 DLC=5 DATA=00 00 00 00 01
...
```

## Photos of my hardware

