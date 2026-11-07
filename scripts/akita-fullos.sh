#!/bin/bash
# Mac: copia las herramientas al teléfono (SSH ya listo) y prepara la sesión del macOS completo; lanza la app dada.
# Uso: akita-fullos.sh ["/ruta/de/la/app/en/el/sistema/completo"]
B=/Volumes/datos2/unixnu-akita; C="-F $B/scratch/sshkeys/ssh_config"
scp -O -q $C $B/tools/chroot/akita-fullos-session.sh $B/tools/chroot/nilguard.dylib akita:/private/var/tmp/ak/ || exit 1
ssh $C akita "bash /private/var/tmp/ak/akita-fullos-session.sh ${1:+\"$1\"}"
