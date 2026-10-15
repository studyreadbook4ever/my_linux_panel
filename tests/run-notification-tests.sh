#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/mypanel-notification-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -O1 \
    tests/notification_center_test.c notification_center.c -I. \
    -o "$build_dir/notification-center-test" $(pkg-config --cflags --libs gtk+-3.0)
# Only the display is shared. The service and all notification data live on a
# disposable bus; never point this binary at the user's notification daemon.
# Do not include standard session service directories: an AUTO_START watcher
# must not launch the installed daemon, which would share the user's log file.
cat > "$build_dir/bus.conf" <<'EOF'
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
dbus-run-session --config-file "$build_dir/bus.conf" -- env MYPANEL_NOTIFICATION_TEST_BUS=private \
    NO_AT_BRIDGE=1 "$build_dir/notification-center-test" "$@"
