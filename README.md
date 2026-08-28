# T-Deck Brick Breaker

Brick Breaker as a **native app inside Meshtastic** on the LilyGo T-Deck. It appears
in the sidebar next to Maps — this is not a replacement firmware, and your node keeps
running normally while you play.

![sidebar: home, nodes, groups, messages, map, BB, settings]

- Roll the trackball left/right to move the paddle
- Press **ENTER** to launch the ball

## Flashing

Built against Meshtastic **2.7.26.54e0d8d**, target `t-deck-tft`. **LilyGo T-Deck only.**

### 1. Put the T-Deck in download mode — required

Meshtastic's firmware uses TinyUSB, which does not expose the ROM bootloader, so no
flasher can do this for you:

1. Unplug USB.
2. Press and **hold** the centre of the trackball.
3. Plug USB back in while still holding.
4. Release.

The screen stays dark. That is normal and does not mean it failed.

If that does not take, leave USB plugged in, hold the trackball centre, tap **RST**,
release RST, then release the trackball.

### 2. Back up first

Your node's private key lives on the device and cannot be recovered if lost:

```
esptool --port /dev/cu.usbmodemXXXX read-flash 0 0x1000000 tdeck-backup.bin
```

Restore with `write-flash 0 tdeck-backup.bin`.

### 3. Flash

Go to [flasher.meshtastic.org](https://flasher.meshtastic.org), select **T-Deck**, then
upload one of the release binaries:

| File | Flasher option | Effect |
| --- | --- | --- |
| `...-brickbreaker.bin` | **Update** | Writes `app0` only. **Keeps your key, channels and node DB.** Use this one. |
| `...-brickbreaker.factory.bin` | Install from scratch | Full erase. Your node loses its private key and comes back as a new identity. |

Prefer **Update**. Only reach for the factory image if the update path fails.

Do not rename the files — the flasher decides whether a full install is even offered
by checking that the name ends in `.factory.bin`.

## Building from source

This repository is a fork of [meshtastic/device-ui](https://github.com/meshtastic/device-ui)
at commit `1c45ebc`, with the game added on the `brickbreaker` branch. The game itself is:

- `include/graphics/game/BrickBreakerPanel.h`
- `source/graphics/game/BrickBreakerPanel.cpp`
- ~77 lines of wiring in `source/graphics/TFT/TFTView_320x240.cpp`

No generated EEZ Studio code is modified — the launcher button and panel are created at
runtime as siblings of the Map ones, so this rebases cleanly onto newer device-ui commits.

To build, clone `meshtastic/firmware` at `v2.7.26.54e0d8d` and point its device-ui
dependency at your checkout of this repo:

```ini
[device-ui_base]
lib_deps =
	symlink:///path/to/t-deck-brickbreaker
```

Then:

```
pio run -e t-deck-tft
```

## Notes on the hardware

Two things make input on this device awkward, both handled in `BrickBreakerPanel.cpp`:

- `EncoderInputDriver` rate-limits the trackball to one event per 250ms, so a
  per-event paddle step can never feel fast. Each event instead starts a short
  glide that ramps while the roll continues.
- lvgl's encoder handler converts `LV_KEY_LEFT`/`RIGHT` into rotation before any
  widget sees them, and the T-Deck driver separately remaps the trackball's
  horizontal axis onto `LV_KEY_UP`/`DOWN`. The game reads the trackball GPIOs
  directly to sidestep both.

## Licence

GPL-3.0, inherited from meshtastic/device-ui. See [LICENSE](LICENSE).
