#!/bin/sh
# fullos35: bypass loginwindow and launch desktop directly
# Run this via akx after the phone has booted

echo "=== Checking processes ===" > /dev/console
ps -ax | head -40 > /dev/console 2>&1

echo "=== Checking NULLFIX ===" > /dev/console
sysctl kern.version > /dev/console 2>&1

echo "=== Checking user akita ===" > /dev/console
dscl . -list /Users > /dev/console 2>&1
id akita > /dev/console 2>&1

echo "=== Checking loginwindow ===" > /dev/console
ps -ax | grep -i loginwindow > /dev/console 2>&1

echo "=== Checking kcpassword ===" > /dev/console
ls -la /etc/kcpassword > /dev/console 2>&1
ls -la /private/var/db/.AppleSetupDone > /dev/console 2>&1

echo "=== Trying launchctl bootstrap gui/501 ===" > /dev/console
/bin/launchctl bootstrap gui/501 > /dev/console 2>&1
echo "bootstrap result: $?" > /dev/console

echo "=== Launching Finder ===" > /dev/console
/bin/launchctl kickstart -k gui/501/com.apple.Finder > /dev/console 2>&1 || \
  /usr/bin/open -a /System/Library/CoreServices/Finder.app > /dev/console 2>&1
echo "Finder launch result: $?" > /dev/console

echo "=== Done ===" > /dev/console
