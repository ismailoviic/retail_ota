# Firmware 29 — OTA deployment runbook

Status: **not started**. Nothing below has been published yet.

Order: **Gate 0 → USB canary → binary → version.txt**. Never publish `version.txt` first.

---

## How the OTA works (verified against v28, v29 and live GitHub, 26 Oct)

| Fact | Consequence |
|---|---|
| v28 devices read `version.txt` every **10 min**, with a cache-buster | They see a new number almost at once |
| A number **higher** than the installed version triggers the download of `build/esp32.esp32.esp32c3/retail_ota.ino.bin` | The binary name and path must stay exactly that |
| The image digest is checked before the boot partition switches | A truncated or corrupted download leaves the device on 28 |
| Bootloader rollback is compiled in, but the Arduino core marks the new app valid **before `setup()`** | No automatic rollback: once v29 boots, it stays |
| Devices only accept a **higher** number | Rolling back = publishing **v30** (v28 code). Setting `version.txt` to 28 does not downgrade anything |
| v29 checks OTA **after every power-on/RESET**, then every **24 h** | Rollback reaches a v29 device within 24 h, or at once if you power-cycle it |
| v29 publishes over MQTT only (port 1883), OTA still uses HTTPS (port 443) | A broken MQTT path does not block a rollback |
| raw.githubusercontent.com sends `Cache-Control: max-age=300` | Allow up to 5 min before a pushed file is served |
| Git treats the `.bin` as binary (`-text`) | No line-ending corruption on commit |
| Live v28 binary SHA-256 `dee7b936df5ed7a1107e563b2f8afe110266103022311dd688c335a524e9401d` (1,297,920 B) | Matches the repo; this is the known-good fallback |
| v29 binary: 1,307,520 B of a 1,310,720 B OTA slot | Fits, with 3.2 KB to spare |

---

## Gate 0 — go / no-go (all must be ticked)

- [ ] **Bridge ready.** A subscriber on `storeyes/+/coffee-counter`, QoS 1, writes each message to the Supabase table the dashboard reads. It maps the grouped payload (`device`, `reading`, `level`, `sensor`, `power`, `schedule`, `network`, `diagnostics`) and dedupes on `reading.reading_id`. **Without it the dashboard goes blank after the update.**
- [ ] **Café network allows outbound TCP 1883** to `18.100.207.236`. Test from the café's own Wi-Fi (guest networks often allow only 80/443).
- [ ] **Rollback image v30 built and tested over USB**: v28 source with `CURRENT_VERSION = 30`, exported as `.bin`.
- [ ] **Push access** to `ismailoviic/retail_ota` (main branch).
- [ ] **Local copy of the v28 binary** kept outside the repo (it is also in git history, commit `0f04667`).

---

## Step 1 — USB canary (one device, 24 h)

Use the **test board** (`e2a880ac-…`). A board that was never provisioned would store the production ID `4f974379-…` compiled into v29 and impersonate the production device.

1. Arduino IDE → open `retail_ota_v29/retail_ota_v29.ino` → board settings as v28 (ESP32C3 Dev Module, USB CDC on boot enabled, 160 MHz, QIO 80 MHz, 4 MB, Default partition) → **Upload**.
   Do **not** flash `retail_ota_v29.ino.merged.bin`: it erases NVS (Wi-Fi credentials and device ID).
2. Serial monitor 115200, expect:
   ```
   AI Coffee ESP32-C3 firmware 29; device e2a880ac-…
   Connected: <ssid>; RSSI … dBm; connect … ms
   MQTT attempt 1/3: acknowledged; error 0; CONNACK code 0
   [OTA] Installed=29; published=28
   [OTA] No newer firmware.
   ```
3. Watch the broker from a PC:
   ```bash
   mosquitto_sub -h 18.100.207.236 -p 1883 -u storeyes -P 12345 -t "storeyes/+/coffee-counter" -q 1 -v
   ```
4. Confirm the row appears in Supabase / on the dashboard.
5. Leave it 24 h. Check: `schedule.schedule_reason` moves through `refill_hold`, `high_level`, `draining`, `night_stable`; about 37 wakes per day; no `previous_publish_result` below 0 except isolated network drops.

**Stop here if any check fails.**

---

## Step 2 — publish the binary only (harmless)

Devices ignore the binary while `version.txt` stays at 28.

```bash
cp retail_ota_v29/build/esp32.esp32.esp32c3/retail_ota_v29.ino.bin build/esp32.esp32.esp32c3/retail_ota.ino.bin
```
```bash
git add build/esp32.esp32.esp32c3/retail_ota.ino.bin retail_ota_v29/retail_ota_v29.ino retail_ota_v29/build_opt.h retail_ota_v29/src retail_ota_v29/DEPLOY_v29.md
```
```bash
git commit -m "Firmware 29 binary (version.txt unchanged)"
```
```bash
git push origin main
```

Wait 5 minutes, then verify GitHub serves exactly this file (the two hashes must match):

```bash
curl -sS "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32c3/retail_ota.ino.bin?check=$RANDOM" | sha256sum; sha256sum retail_ota_v29/build/esp32.esp32.esp32c3/retail_ota_v29.ino.bin
```

Mismatch → `git revert HEAD`, push, and start Step 2 again. Do not continue.

---

## Step 3 — publish the version (separate commit)

Plain integer, no `v`, no newline needed:

```bash
printf '29' > version.txt
```
```bash
git commit -am "Publish firmware 29" && git push origin main
```

Optional check that the new number is served:

```bash
curl -sS "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt?check=$RANDOM"
```

---

## Step 4 — verify the fleet (first hour)

- [ ] Within ~15 min of `version.txt` changing, each device stops posting firmware-28 rows to Supabase.
- [ ] Each device's first MQTT message arrives with `"firmware_version":29`.
- [ ] Supabase rows from the bridge show `device.firmware_version = 29`.
- [ ] No device silent for more than 1 h after the switch.

---

## Backup plan

### A — Download fails (devices stay on 28)
Symptom: firmware-28 rows keep arriving; devices retry every 10 min, which costs battery.
Action: fix and re-upload the binary (Step 2). If it cannot be fixed quickly, set `version.txt` back to `28` to stop the retries.

### B — v29 runs but data stops (broker down, port 1883 blocked, bridge wrong)
Symptom: no firmware-29 messages on the broker or no rows in Supabase.
Action — publish v30 (= v28 code):
1. Copy the tested v30 binary to `build/esp32.esp32.esp32c3/retail_ota.ino.bin`, commit, push, verify the hash (as in Step 2).
2. `printf '30' > version.txt`, commit, push.
3. v29 checks OTA every 24 h. To roll back at once, **power-cycle the device** (switch off/on or RESET): v29 checks OTA right after every power-on.
4. v30 posts to Supabase again with `firmware_version` 30.

### C — v29 cannot reach Wi-Fi or reboots in a loop
Symptom: device silent on both MQTT and Supabase; OTA cannot reach it.
Action — USB recovery on site:
1. Open `retail_ota.ino` (v28) or the v30 sketch in the Arduino IDE, same board settings, **Upload**. This writes bootloader, partition table, `boot_app0` and app only; NVS (Wi-Fi credentials, device ID) is kept.
2. Or with arduino-cli from an existing build folder:
   ```bash
   arduino-cli upload -p COMx --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default" --input-dir build/esp32.esp32.esp32c3
   ```
3. Never flash a `merged.bin` on a deployed device.
4. If the Wi-Fi was lost: hold for the portal "AI Coffee" / `12345678` (README, user section).

### D — Wrong file pushed
Symptom: the Step 2 hash check fails.
Action: `git revert HEAD`, push, redo Step 2. `version.txt` must still say 28.

### Never
- Publish `version.txt` before the binary is verified.
- Use `28` in `version.txt` to roll back v29 devices (it only stops v28 devices from updating).
- Reuse a version number that a device has already installed.

---

## Optional hardening (not in this build)

v29 could delay accepting itself until its first MQTT PUBACK (`verifyRollbackLater()` returning true, then `esp_ota_mark_app_valid_cancel_rollback()` after the publish). If the first wake cannot publish, the bootloader returns to v28 by itself. While `version.txt` is 29 the restored v28 would download v29 again 10 min later, so lower `version.txt` to 28 if devices start flipping back.
