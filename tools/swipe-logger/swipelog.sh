#!/bin/zsh
# Background swipe logger: saves the Dock's gesture lines and the app's own lines
# to a size-limited file. Reads only; it never blocks or changes an event.
# Usage: swipelog.sh install|uninstall|pause|resume|status|mark [text]|save|view|run

SELF="${0:A}"   # this script; $0 inside a function is the function name
LABEL="com.sagarEmn.InstantSpaceSlider.swipelog"
DIR="$HOME/Library/Logs/InstantSpaceSlider"
FILE="$DIR/swipes.log"
SUPPORT="$HOME/Library/Application Support/InstantSpaceSlider"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"
MAX_BYTES=5242880   # 5 MB per file; swipes.log plus 3 older ones, 20 MB at most
PRED='((process == "Dock" AND (category == "SystemGestureManager" OR category == "missioncontrol")) OR subsystem == "com.sagarEmn.InstantSpaceSlider") AND NOT eventMessage BEGINSWITH "Escape" AND NOT eventMessage BEGINSWITH "F3 "'

stamp() { /bin/date '+%F %T'; }

run() {
    mkdir -p "$DIR"
    print -- "$(stamp) --- logger started" >> "$FILE"
    /usr/bin/log stream --style compact --predicate "$PRED" | /usr/bin/awk -v file="$FILE" -v max=$MAX_BYTES '
    BEGIN { cmd = "/usr/bin/stat -f %z \"" file "\""; if ((cmd | getline s) > 0) size = s + 0; close(cmd) }
    /^Filtering the log data/ || /^Timestamp/ { next }
    {
        print >> file; fflush(file); size += length($0) + 1
        if (size >= max) {
            close(file)
            system("cd \"$(dirname \"" file "\")\" && mv -f swipes.log.2 swipes.log.3 2>/dev/null; mv -f swipes.log.1 swipes.log.2 2>/dev/null; mv -f swipes.log swipes.log.1")
            size = 0
        }
    }'
}

install() {
    # Run from a copy, so switching git branches never removes the running script
    mkdir -p "$SUPPORT" "$DIR" "${PLIST:h}"
    cp -f "$SELF" "$SUPPORT/swipelog.sh"
    chmod +x "$SUPPORT/swipelog.sh"
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/zsh</string>
        <string>$SUPPORT/swipelog.sh</string>
        <string>run</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>10</integer>
    <key>ProcessType</key><string>Background</string>
    <key>StandardErrorPath</key><string>$DIR/logger-errors.log</string>
</dict>
</plist>
EOF
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null
    launchctl bootstrap "$DOMAIN" "$PLIST" && echo "Installed and running. Log: $FILE"
}

case "${1:-run}" in
    run) run ;;
    install) install ;;
    uninstall)
        launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null
        rm -f "$PLIST" "$SUPPORT/swipelog.sh"
        echo "Uninstalled. Old logs are kept in $DIR" ;;
    pause)
        launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null
        print -- "$(stamp) --- logger paused" >> "$FILE"
        echo "Paused; nothing new is logged. It starts again at next login, or with: $0 resume" ;;
    resume)
        launchctl bootstrap "$DOMAIN" "$PLIST" && echo "Resumed." ;;
    status)
        if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then echo "Running."; else echo "Not running."; fi
        ls -lh "$DIR"/swipes.log* 2>/dev/null ;;
    mark)
        shift
        print -- "$(stamp) === MARK: ${*:-stuck here}" >> "$FILE"
        echo "Marked." ;;
    save)
        out="$HOME/Desktop/swipes-$(/bin/date '+%Y%m%d-%H%M%S').log"
        cat "$DIR"/swipes.log.3 "$DIR"/swipes.log.2 "$DIR"/swipes.log.1 "$FILE" 2>/dev/null > "$out"
        echo "Saved to $out" ;;
    view) tail -n 50 -f "$FILE" ;;
    *) echo "Usage: $0 install|uninstall|pause|resume|status|mark [text]|save|view|run"; exit 1 ;;
esac
