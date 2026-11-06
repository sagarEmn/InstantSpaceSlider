# Swipe logger

A background log for finding out why a swipe got stuck. It runs all the time, starts at login, and restarts itself if it stops. It only reads the system log. It never blocks or changes an event, so it can't cause a stuck swipe itself.

- **Log file:** `~/Library/Logs/InstantSpaceSlider/swipes.log`
- **Size:** at most 20 MB. When the file reaches 5 MB, it moves to `swipes.log.1` and a new one starts. The 3 newest old files are kept.
- **Cost:** about 4 MB of memory and no CPU while idle.

Run all commands from the repo folder.

## Set up once

```sh
./tools/swipe-logger/swipelog.sh install
```

This copies the script to `~/Library/Application Support/InstantSpaceSlider/` so that switching git branches doesn't stop it.

## When a swipe gets stuck

1. Mark the moment. You don't need to stop anything:
   ```sh
   ./tools/swipe-logger/swipelog.sh mark "swiped right on desktop 2, nothing moved"
   ```
2. Ask Claude to read `~/Library/Logs/InstantSpaceSlider/swipes.log` around the mark, and describe the movement you made.

If you can't run the mark command right away, just note the time.

## Other commands

| Command | What it does |
|---|---|
| `status` | Shows whether the logger is running, and the size of each log file |
| `view` | Shows the last 50 lines, then new lines as they come in (Ctrl-C to quit) |
| `save` | Copies all the logs into one file on your Desktop, to keep or share |
| `pause` | Stops logging, so the log ends where the problem happened. It starts again at next login |
| `resume` | Starts logging again after a pause |
| `uninstall` | Removes the background service. The logs are kept |

For example: `./tools/swipe-logger/swipelog.sh pause`.

## Reading the log

Each line starts with the date and time, then the process.

- **`Dock … SystemGestureManager`**: the Dock's own view of the gesture: `active` when it starts, `ended` when it closes. If a swipe has an `active` line with no `ended` after it, the Dock stayed stuck in that swipe.
- **`InstantSpaceSlider … [swipe]`**: the app's steps for each left/right swipe:
  - `began`: a swipe started, with the desktops that are blocked at the edges
  - `ended` or `cancelled`: you lifted your fingers. Shows the progress, the velocity and the chosen direction
  - `finish`: the fast finish started, going from the progress to the target
  - `finish done`: the finish ended and the held release was sent to the Dock. If a `finish` has no `finish done` after it, the Dock never got its release
  - `no finish` or `ended while not tracking`: the app passed the swipe to the Dock without finishing it
  - `event tap disabled`: macOS turned off the app's event tap for a moment, so events were missed
- **`[mission-control-click]`**: whether a sideways swipe started inside or outside Mission Control.

Only the app version built from this branch writes the `[swipe]` lines.
