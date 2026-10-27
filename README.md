# InstantSpaceSlider

Fast workspace switching on macOS: plays the native slide animation at an adjustable speed (or skips it entirely), so you can still see which way you moved.

Forked from [jurplel/InstantSpaceSwitcher](https://github.com/jurplel/InstantSpaceSwitcher), the original project, by jurplel (MIT license).

https://github.com/user-attachments/assets/037422c9-3fb7-41cd-8da7-58d28c4c8eff

## Features

- Does not require disabling SIP
- Uses native macOS spaces
- Free
- Adjustable slide length (Settings > Animation Speed), from a visible slide to instant, with focus landing as soon as the slide ends
- Trackpad swipe modes: instant, or follow your fingers (drag partway, back and forth) and snap quickly on release
- Optional fast Mission Control: swipe up / F3 open and close it with the same short slide (Settings > Speed up Mission Control)

A simple CLI is provided (`InstantSpaceSlider.app/Contents/MacOS/ISSCli --help`). It uses the speed set in the app; pass `--duration <ms>` to override it (e.g. `ISSCli --duration 0 right` for instant).


## Versions: usable-v2 and usable-v3

Both are tagged in this repo. To switch between them, check out the tag, rebuild and relaunch:

```sh
git checkout usable-v3   # or usable-v2
./dist/build.sh
pkill -x InstantSpaceSlider; open build/InstantSpaceSlider.app
```

After a rebuild, macOS may drop the Accessibility permission. If swipes or shortcuts stop working, remove the app from System Settings > Privacy & Security > Accessibility, add it back, and relaunch it.

| | usable-v2 | usable-v3 |
|---|---|---|
| Switching spaces (shortcuts, swipes) | Fast slide | Fast slide (same) |
| Swipe up / F3 to open Mission Control, swipe down to close | Fast | Fast (same) |
| Swiping left/right inside Mission Control | Native, snappy release | Same |
| Clicking a desktop in Mission Control's Spaces bar | macOS's own zoom: smooth but slow | Fast: Mission Control closes instantly, then cuts to the clicked desktop |
| Clicking a window in Mission Control | macOS's own zoom | macOS's own zoom (unchanged) |

What isn't fast or perfect in usable-v3:

- When you click a desktop other than the highlighted one, the highlighted desktop flashes for a split second before the clicked one appears. Mission Control can only close onto the highlighted desktop, and there's no public way to move the highlight first.
- Only clicks after opening Mission Control with the three-finger swipe or F3 (with "Speed up Mission Control" on) are sped up. Mission Control opened another way (hot corner, Dock) behaves natively.
- Window clicks inside Mission Control still use the normal animation.

## Installation

### Downloads

Pre-built binaries are available through Github Releases [here](https://github.com/sagarEmn/InstantSpaceSlider/tags).

### Build from source

```sh
git clone https://github.com/sagarEmn/InstantSpaceSlider
cd InstantSpaceSlider
./dist/build.sh
open ./build/InstantSpaceSlider.app
```

### Troubleshooting / First time startup

1. When opening the app the first time you are likely to run into any of the following warnings:

> "Apple could not verify InstantSpaceSlider.app is free of malware"

> "App is damaged and can’t be opened"

> "App can't be opened because the developer cannot be verified"

Please follow the instructions [here](https://wiki.hacks.guide/wiki/Open_unsigned_applications_on_macOS_Sequoia_and_newer) for instructions on how to allow the app to run[^1].

2. Afterward you will get a warning that InstantSpaceSlider wants to use accessibility features. You have to allow this.

3. Then open the app from the Applications folder and the configuration window should appear.

## Background
When I first bought a high refresh rate monitor, around ~2018, I could tell that the space switching animation was longer because it had scaled with the refresh rate. Because of this, I eventually stopped using spaces altogether, and have been looking for a solution ever since. 

The workaround in this project is to create a synthetic trackpad gesture with an artificially high velocity. This effectively skips the animation.

If you work at Apple, and your team owns the space switching animation, please fix this long-standing bug[^2] (and let us disable the animation natively, please).

[^1]: This happens because the app is not signed, which requires a costly Apple Developer account
[^2]: And I know for a fact there is a rdar for this

