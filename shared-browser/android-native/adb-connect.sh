#!/bin/sh
# Connect this host's adb to the Waydroid device and wait until Android has
# finished booting. Python-free, one line of shell: it runs as a service.
export PATH=/opt/android-sdk/platform-tools:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
adb start-server >/dev/null 2>&1 || true
i=0
while [ "$i" -lt 60 ]; do
  adb connect 192.168.240.112:5555 >/dev/null 2>&1 || true
  if [ "$(adb -s 192.168.240.112:5555 shell getprop sys.boot_completed 2>/dev/null | tr -d '
')" = "1" ]; then
    exit 0
  fi
  i=$((i + 1))
  sleep 5
done
exit 0
