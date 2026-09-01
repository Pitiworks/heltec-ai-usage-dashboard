# Hardware

## Board

**Heltec Wireless Paper V1.2**

- MCU: ESP32-S3
- Display: 2.13" black/white E-Ink, 250x122 px effective drawing area (landscape)
- Connectivity used: WiFi (2.4 GHz)
- Flash: 8 MB
- USB: exposed as a serial port (e.g. `/dev/ttyUSB0` on Linux) for flashing and monitoring

- Example hardware source: [Heltec Wireless Paper V1.2 on AliExpress](https://de.aliexpress.com/item/1005009400769852.html)

The board is an all-in-one unit — MCU, E-Ink panel, and power management are on
one PCB. No external wiring is required; the `heltec-eink-modules` library
talks to the panel directly once the correct build flag is set (see
[`../README.md`](../README.md#firmware)).

## Why `EInkDisplay_WirelessPaperV1_2`

Heltec has shipped several hardware revisions of the Wireless Paper board
(V1, V1.1, V1.1.1, V1.2), each wired to a slightly different E-Ink panel
controller. The `heltec-eink-modules` library exposes one display class per
revision. This project targets **V1.2**, which maps to display controller
`E0213A367` in the library. If you have an older revision, swap
`EInkDisplay_WirelessPaperV1_2` for the matching class in
[`../firmware/src/main.cpp`](../firmware/src/main.cpp) — see the library's own
docs for the full list.

## Identifying your board revision

The revision is usually printed on the back of the board, near the FCC/CE
markings. If in doubt, check the `heltec-eink-modules` library's own
`docs/Identification/` photos (in the library source, not part of this repo)
to compare.

## Power

USB power is enough for continuous operation (polling the API and refreshing
the display every 60 seconds). No battery is required, though the board does
support one — see the `heltec-eink-modules` library docs for deep-sleep /
battery guidance if you want to run it untethered.

## images/

Placeholder directory for hardware/wiring photos and a photo of the running
display. Add your own images here and reference them from this file or the
main README, e.g.:

```markdown
![Wireless Paper V1.2 front](images/board-front.jpg)
```
