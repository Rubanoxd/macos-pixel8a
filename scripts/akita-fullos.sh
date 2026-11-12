#!/bin/bash
# Mac side: copies the tools to the phone (SSH already up), prepares the full-macOS session and launches the given app.
# Uso: akita-fullos.sh ["/ruta/de/la/app/en/el/sistema/completo"]
B=/Volumes/datos2/unixnu-akita; C="-F $B/scratch/sshkeys/ssh_config"
scp -O -q $C $B/tools/chroot/akita-fullos-session.sh $B/tools/chroot/nilguard.dylib akita:/private/var/tmp/ak/ || exit 1
ssh $C akita "bash /private/var/tmp/ak/akita-fullos-session.sh ${1:+\"$1\"}"
