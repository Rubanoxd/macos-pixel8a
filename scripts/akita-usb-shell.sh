#!/bin/bash
# SSH access to the Pixel 8a's macOS over the USB cable (CDC-ECM gadget of AkitaUsbGadget.h,
# boot-arg akita-usb-gadget=2; the kext puts 169.254.7.2/16 on the phone's en0).
#
#   ./akita-usb-shell.sh setup   leaves sshd running on the phone (own keys in scratch/sshkeys)
#                                then: ssh -F /Volumes/datos2/unixnu-akita/scratch/sshkeys/ssh_config akita
#
# How it starts: if ssh does not answer yet, it types through remote control (akita-live.py, UDP 5141) into the focused
# Terminal the command  nc -l 2323|bash|nc -l 2324  and, through that one-shot channel (input on 2323, output on
# 2324), installs the keys and starts sshd. 2323/2324 are not probed with nc -z: each nc -l accepts ONE connection.
# Recovery has no sftp-server: to copy files use  scp -O -F .../ssh_config akita:/path .
set -u
PHONE=169.254.7.2
BASE=/Volumes/datos2/unixnu-akita
KEYS=$BASE/scratch/sshkeys
WORK=$BASE/scratch/shell
mkdir -p "$WORK" "$KEYS"
IF=$(for i in $(ifconfig -l); do ifconfig "$i" 2>/dev/null | grep -q "ether 02:a5:1a:00:00:01" && echo "$i"; done | head -1)
[ -z "$IF" ] && { echo "can't find the Pixel's USB interface (ether 02:a5:1a:00:00:01)"; exit 1; }

[ -f "$KEYS/host_ed25519" ] || ssh-keygen -q -t ed25519 -N "" -C akita-host -f "$KEYS/host_ed25519"
[ -f "$KEYS/client_ed25519" ] || ssh-keygen -q -t ed25519 -N "" -C akita-client -f "$KEYS/client_ed25519"
cat > "$KEYS/ssh_config" <<EOF
Host akita
  HostName $PHONE
  User root
  IdentityFile $KEYS/client_ed25519
  UserKnownHostsFile $KEYS/known_hosts
  StrictHostKeyChecking accept-new
  ConnectTimeout 6
  BindInterface $IF
EOF
sshok() { ssh -F "$KEYS/ssh_config" -o BatchMode=yes akita 'echo SSHOK' 2>/dev/null | grep -q SSHOK; }

if [ "${1:-}" != "setup" ]; then sed -n '2,11p' "$0"; exit 0; fi
if sshok; then echo "sshd ya responde"; exit 0; fi

if [ "${2:-}" != "--no-type" ]; then
  python3 "$BASE/akita-live.py" run 'nc -l 2323|bash|nc -l 2324'
  sleep 8
fi
out="$WORK/bootstrap-$(date +%H%M%S).txt"; fifo="$WORK/in.fifo"
rm -f "$fifo"; mkfifo "$fifo"
nc -b "$IF" "$PHONE" 2324 > "$out" & reader=$!
sleep 1
nc -b "$IF" "$PHONE" 2323 < "$fifo" & writer=$!
exec 3> "$fifo"
cat >&3 <<EOF
exec 2>&1
mkdir -p /private/var/tmp/ak; chmod 700 /private/var/tmp/ak
cat > /private/var/tmp/ak/host <<'EOK'
$(cat "$KEYS/host_ed25519")
EOK
echo '$(cat "$KEYS/client_ed25519.pub")' > /private/var/tmp/ak/authorized_keys
chmod 600 /private/var/tmp/ak/host /private/var/tmp/ak/authorized_keys
nohup /usr/sbin/sshd -D -e -f /dev/null -h /private/var/tmp/ak/host -o AuthorizedKeysFile=/private/var/tmp/ak/authorized_keys -o PermitRootLogin=prohibit-password -o StrictModes=no -o UsePAM=no -o PidFile=none -p 22 > /private/var/tmp/ak/sshd.log 2>&1 &
sleep 1; echo SETUP-DONE
EOF
for _ in $(seq 1 15); do sshok && break; sleep 1; done
exec 3>&-
sleep 1; kill $reader $writer 2>/dev/null
if sshok; then
  ssh -F "$KEYS/ssh_config" akita 'echo "sshd ready: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -r)"'
else
  echo "sshd no responde; salida del canal:"; cat "$out"; exit 1
fi
