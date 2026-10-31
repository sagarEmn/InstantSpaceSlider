# InstantSpaceSlider

Fast workspace switching on macOS: plays the native slide animation at an adjustable speed (or skips it entirely), so you can still see which way you moved.

Forked from [jurplel/InstantSpaceSwitcher](https://github.com/jurplel/InstantSpaceSwitcher), the original project, by jurplel (MIT license).

https://github.com/user-attachments/assets/037422c9-3fb7-41cd-8da7-58d28c4c8eff

## The goal: how swiping should feel

Moving between desktops on macOS is slow, and Reduce Motion removes the slide, so you can't tell which way you moved. This app keeps the movement and removes the wait. A swipe should be as fast or as slow as your fingers, and it should never leave you waiting for an animation after you lift them.

A **direct swipe** is a three-finger left/right swipe made on a desktop or full-screen app while Mission Control is closed. It moves between desktops and full-screen apps. This is the mechanism a direct swipe should have:

1. **It follows your fingers.** While your fingers are down, the view moves with them. Move slowly and it moves slowly; move fast and it moves fast. You can go back and forth as many times as you like.
2. **Lifting decides where it goes.** The last direction your fingers were moving picks the destination, not how far you got. You don't have to swipe all the way across: a short swipe toward the next desktop is enough.
3. **It snaps quickly on release.** From wherever you lifted, the view finishes fast (a full slide takes `slideDurationMs`, and a part-way finish takes proportionally less), then focus lands immediately. There is no slow spring tail.
4. **It snaps back if you came back.** If your last movement was toward where you started, or there's no desktop on that side, it returns quickly to where you started.

A **Mission Control swipe** is a three-finger left/right swipe while Mission Control is open. It moves between desktops in Mission Control's view and uses the same mechanism.

## Current version: usable-v5.1-macos27.2-golden-gate on macOS 27.2 (Golden Gate)

### What works

- **Direct swipes:** the full mechanism above: follows your fingers, decides by last direction, snaps fast on release, and snaps back fast. Confirmed with real swipes.
- **Mission Control swipes:** follow your fingers and snap fast on release, even from part of the way. Confirmed with real swipes.
- **Opening Mission Control with a three-finger swipe up:** fast and snappy, and it follows your fingers: swipe up slowly and it moves slowly, swipe fast and it moves fast. This is what we want. It's macOS's own behaviour; the app doesn't change it on this version.
- **Shortcuts and the CLI:** a visible fast slide of `slideDurationMs`.

### Problem in this version: what's still slow (default macOS speed)

- **Clicking a desktop in Mission Control's Spaces bar:** default speed. The fast clicks from usable-v3 to usable-v4.2 are switched off on macOS 27.
- **The Mission Control key (F3):** not handled by the app, so it runs at default speed.
- **Swiping down to close Mission Control:** not handled by the app. It was slow when last checked.

### Known problem in this version: swiping past the first or last desktop in Mission Control

With Mission Control open, swiping fully past the first or last desktop shows an empty black desktop for under a second, then the desktop slides back in from the side. Mission Control itself stays normal. It should instead move only a little and spring back, with no black gap.

### Not tested yet with real swipes on this version

- Direct swipes past the first or last desktop
- Lifting while moving back when macOS doesn't report a cancelled swipe
- Snap-back in Mission Control, and the + button, x button and drag-to-reorder

### How direct swipes work on macOS 27

On macOS 27 the Dock reads a swipe from the IOHID payload embedded in each event. Editing an event's fields doesn't change that payload.

1. The real swipe passes through untouched, so the Dock follows your fingers natively.
2. On release, the app holds back the real release event.
3. It sends quick frames, edited from the last real Changed event, that carry the view from where you lifted to the destination. Real gesture (type 29) events are blocked during this short finish.
4. It sends the held real release last, edited to match, so the Dock closes the gesture. This event must always arrive, or the Dock stays stuck in the swipe.

Mission Control swipes use the same finish. Things learned on the way:

- usable-v4.2 raised the release velocity in the event's fields. On macOS 27 that has no effect, because the Dock reads the payload.
- Raising the velocity in the payload isn't enough either. On direct swipes it reached the Dock, but the visible finish stayed slow.
- Mission Control is detected through `WindowManager`'s full-screen window on layer 19, because the Dock no longer owns those windows.

## Features

- Does not require disabling SIP
- Uses native macOS spaces
- Free
- Adjustable slide length (Settings > Animation Speed), from a visible slide to instant, with focus landing as soon as the slide ends
- Trackpad swipe modes: instant, or follow your fingers (drag partway, back and forth) and snap quickly on release
- Optional fast Mission Control on macOS 26: swipe up / F3 open and close it with the same short slide (Settings > Speed up Mission Control)

A simple CLI is provided (`InstantSpaceSlider.app/Contents/MacOS/ISSCli --help`). It uses the speed set in the app; pass `--duration <ms>` to override it (e.g. `ISSCli --duration 0 right` for instant).


## Versions: latest to oldest

All are tagged in this repo. `usable-v5.1-macos27.2-golden-gate` is the latest and the one to use on macOS 27 (Golden Gate); usable-v4.2 is the latest for macOS 26. To switch between them, check out the tag, rebuild and relaunch:

```sh
git checkout usable-v5.1-macos27.2-golden-gate   # or usable-v5-macos27-golden-gate, usable-v4.2, usable-v4.1, usable-v4, usable-v3, usable-v2
./dist/build.sh
pkill -x InstantSpaceSlider; open build/InstantSpaceSlider.app
```

After a rebuild, macOS may drop the Accessibility permission. If swipes or shortcuts stop working, remove the app from System Settings > Privacy & Security > Accessibility, add it back, and relaunch it.

Newest is on the left. "Same" means the same as the older version to its right.

| | usable-v5.1-macos27.2-golden-gate (macOS 27.2) | usable-v5-macos27-golden-gate (macOS 27) | usable-v4.2 | usable-v4.1 | usable-v4 | usable-v3 | usable-v2 |
|---| --- |---|---|---|---|---|---|
| Switching spaces (shortcuts, swipes) | Same | Fast slide; swipes follow your fingers and snap on release | Same as v3 | Same | Same | Same | Fast slide |
| Swipe up / F3 to open Mission Control, swipe down to close | Same | Left to macOS: swipe up is fast, the rest are default speed | Same as v3 | Same | Same | Same | Fast |
| Swiping left/right inside Mission Control | Follows your fingers and snaps on release. **Bug:** brief black gap past the first or last desktop | **Slow:** default speed, no snap | Fixed | Fixed | Same, but **sometimes jumps** to the last desktop and back | Same as v2 | Native, snappy release |
| Clicking a desktop in Mission Control's Spaces bar | Same | **Slow:** left to macOS | Fixed | Fixed | Fast, v3 bug fixed. **Bug:** rarely opens the wrong desktop after a black screen; clicks in the gaps beside a thumbnail are slow | Fast. **Bug:** sometimes slow (macOS speed), often on the first click after a pause | macOS's own zoom: smooth but slow |
| Add-desktop (+) button | Same | Left to macOS | Works | Works | Works | Works | Works |
| Close (x) button on a desktop thumbnail | Same | Left to macOS | Works | **Opens the desktop instead** | **Opens the desktop instead** | **Opens the desktop instead** | Works |
| Dragging desktops in the Spaces bar to reorder them | Same | Left to macOS | Works | **Broken** | **Broken** | **Broken** | Works |
| Clicking a window in Mission Control | Same | Left to macOS | Same as v3 | Same | Same | Same | macOS's own zoom |
| Click log | Same | No clicks are handled, so none are logged | Yes | Yes | Yes (see below) | No | No |

What isn't fast or perfect from usable-v3 to usable-v4.2:

- When you click a desktop other than the highlighted one, the highlighted desktop flashes for a split second before the clicked one appears. Mission Control can only close onto the highlighted desktop, and there's no public way to move the highlight first.
- Only clicks after opening Mission Control with the three-finger swipe or F3 (with "Speed up Mission Control" on) are sped up. Mission Control opened another way (hot corner, Dock) behaves natively.
- Window clicks inside Mission Control still use the normal animation.
- In usable-v4.2, clicks right at a thumbnail's top-left corner, where the close (x) button appears, go to macOS at its normal speed, even on full-screen apps' thumbnails.

What changed in each version:

- **usable-v5.1-macos27.2-golden-gate:** Mission Control swipes on macOS 27.2 (Golden Gate) follow your fingers and snap fast on release, like direct swipes. Known problem: a brief black gap when swiping past the first or last desktop in Mission Control.
- **usable-v5-macos27-golden-gate:** first version for macOS 27.2 (Golden Gate). Direct swipes follow your fingers and snap fast on release again, and shortcuts and the CLI play the fast slide. Mission Control speedups weren't back yet.
- **usable-v4.2:** dragging desktops to reorder them works again (the press is handed to macOS as soon as you start dragging), and the close (x) button on a thumbnail works.
- **usable-v4.1:** fixed more cases where the app missed that Mission Control was open: sideways swipes jumping to the last desktop and back, and a click opening the wrong desktop after a black screen. Clicks in the gaps beside a thumbnail open that desktop, and the add-desktop (+) button is left alone.
- **usable-v4:** fixed the v3 slow clicks. The app sometimes didn't notice that Mission Control was open, and some clicks landed on a part inside the thumbnail rather than the thumbnail itself.

### Click log (usable-v4 to usable-v4.2)

Each click in Mission Control logs whether it was sped up (`FAST`) or handed to macOS (`NATIVE`, with the reason). To watch it live:

```sh
/usr/bin/log stream --predicate 'subsystem == "com.sagarEmn.InstantSpaceSlider"'
```

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

