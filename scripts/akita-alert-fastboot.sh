#!/bin/bash
# Aviso para que el usuario ponga el Pixel en fastboot: un reggaetón aleatorio de la biblioteca de Apple Music
# y una frase en castellano. Uso: akita-alert-fastboot.sh ["mensaje"]
MSG=${1:-"Pon el Pixel en fastboot, por favor."}
osascript <<'OSA' >/dev/null 2>&1
tell application "Music"
  set picks to (every track of library playlist 1 whose genre contains "eggaet" or genre contains "atin" or genre contains "rbano")
  if (count of picks) > 0 then
    play some item of picks
  else
    play some track of library playlist 1
  end if
end tell
OSA
say -v Eddy "$MSG"
