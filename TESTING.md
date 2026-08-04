# Badge Release Sign-Off

Record badge IDs, firmware versions, and failures while running this list. A
compile is not a substitute for the hardware rows below.

## Automated checks

```sh
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M' \
                  --build-property upload.maximum_size=2162688 \
  --build-path .build/arduino arduino/badge

python3 -m py_compile tools/*.py
sh -n tools/flash_images.sh
python3 tools/mass_flash.py --build-only
python3 tools/mass_flash.py --no-build --once
```

The final command requires a connected badge and performs a complete flash,
including partition table, OTA metadata, factory recovery, main firmware, and
LittleFS artwork. `--build-only` must not open or reset any connected badge.

## First-boot and display

- [ ] MIDWESTCOAST 2026 is front and center during boot.
- [ ] Cyan/amber tunnel has visible depth and moves without a wrap jump.
- [ ] Splash exits after 3 seconds; tap and each edge swipe also dismiss it.
- [ ] Nametag, slideshow, five procedural scenes, and every stored image draw.
- [ ] Two NTP-synced badges show the same slideshow item and animation pose.
- [ ] Brightness, battery icon, keyboard editing, menu paging, and factory-reset
      confirmation work.
- [ ] Hold home for 3 seconds shows the credits; touch returns.

## WiFi and MQTT UX

- [ ] With a wrong WiFi password, touch, animation, menus, and serial remain
      responsive while status reports failure.
- [ ] With an unreachable MQTT host, repeat the same responsiveness checks for
      at least 90 seconds, covering retries and backoff.
- [ ] Correcting broker/port credentials retries promptly and connects.
- [ ] Disconnecting/reconnecting the AP recovers WiFi, SNTP, MQTT subscription,
      retained state, and telemetry without rebooting.
- [ ] Entering **wifi scanner** never freezes the UI; results refresh about
      every 15 seconds and are strongest-first.
- [ ] Scanner rows show SSID, BSSID, channel, auth mode, RSSI, and signal bar;
      up/down reaches every result when more than five networks are visible.
- [ ] The scanner session count increases only for a new BSSID, survives scan
      refreshes, resets on reboot, and does not exceed its 256-entry RAM cap.
- [ ] Scanner pauses an in-progress WiFi join, then resumes it on exit. An
      already-connected badge remains connected while scanning.
- [ ] **wifi scans: PRIVATE** publishes nothing and clears an old retained scan;
      **SHARE** publishes again after the next completed scan.
- [ ] Subscribe to `dc34/badge/+/wifi`; each scan publishes valid JSON with
      `ssid`, `bssid`, `channel`, `rssi`, `auth`, and `secure` for every
      displayed generation.
- [ ] Change name/show locally and remotely; retained state follows both.
- [ ] Signed banner and broadcast commands work; unsigned commands are ignored.

## OTA validation

1. Flash a known baseline version by USB and note its badge ID.
2. Bump `BADGE_VERSION`, build, then run:

   ```sh
   python3 tools/ota_push.py \
     --bin .build/arduino/badge.ino.bin \
     --broker BROKER_IP --badge BADGE_ID
   ```

3. Require main to show the recovery handoff, recovery to show connection and
      download progress, and the updated main to boot and report its new version.
      Confirm settings, identity, WiFi, MQTT, and artwork survived.
4. Publish an update with a deliberately wrong MD5. Require recovery to show a
      verification failure and remain bootable; it must not select the bad main.
5. Repeat with an unreachable URL, an oversized response, a truncated response,
      and a non-ESP32 image. Recovery must show a specific failure in each case.
6. Interrupt power during download. On restart, recovery must retry safely and
      remain intact. Restore the server and complete the update.
7. Flash a test main that resets before `otaConfirmBoot()`. Require the ESP-IDF
      first-boot rollback path to select factory recovery on the next boot.
8. After a successful update, power-cycle and confirm main remains selected.

## Fleet station

- [ ] `python3 tools/flash_gui.py` opens at 1120x720 and remains usable at a
      smaller resized window.
- [ ] Build/art/reflash toggles affect the next run; controls lock during work.
- [ ] Live Bay ignores devices present at launch, then flashes after replug.
- [ ] Flash Connected handles all current compatible ports once.
- [ ] Success, failure, skip, and stop states appear in feed and counters.
- [ ] Four badges can flash concurrently on the intended powered USB hub.
- [ ] A rebooted badge is skipped by MAC; a different badge on the same port is
      enrolled after the unplug grace period.
- [ ] Every flashed badge completes the first-boot and OTA checks above.

## Known release risks

- OTA downloads use HTTP and MD5, not signed firmware images.
- Battery ADC wiring/divider values still require comparison with a multimeter.