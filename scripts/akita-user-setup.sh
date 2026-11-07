#!/bin/bash
# After a fullos boot: create the akita admin user and mark Setup Assistant done (tmpfs state, redo every boot).
cd /Volumes/datos2/unixnu-akita
timeout 150 scratch/akx 'sysadminctl -addUser akita -fullName Akita -password akita -admin 2>&1 | tail -1; touch /private/var/db/.AppleSetupDone; defaults write /Library/Preferences/com.apple.loginwindow autoLoginUser akita; id akita; killall loginwindow' 120
