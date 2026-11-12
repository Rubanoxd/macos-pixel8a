#!/usr/bin/env python3
"""Continuous window drag: one pointer move every 16 ms for ~22 s, sent as one input script per second (UDP datagrams are small).
Usage: drag60.py [seconds]"""
import subprocess, sys, time
secs = int(sys.argv[1]) if len(sys.argv) > 1 else 22
def send(s): subprocess.run(['python3', 'akita-live.py', 'steps', s], capture_output=True)
import os
X0, Y0 = int(os.environ.get('DRAG_X', 440)), int(os.environ.get('DRAG_Y', 62))
if os.environ.get('DRAG_OPEN', '1') == '1':
    send('A72,505_C'); time.sleep(2); send('K11,08'); time.sleep(4)
send('A%d,%d_D' % (X0, Y0)); time.sleep(0.1)
i = 0
# each datagram starts with D: the button state does not survive between datagrams (runSteps' local)
WAIT = int(os.environ.get('DRAG_WAIT', 16))   # ms between moves (16: 60 moves/s)
PER = min(60, 1000 // WAIT)                    # moves per datagram (a longer step list is dropped)
for chunk in range(secs * 1000 // (PER * WAIT)):
    parts = []
    for _ in range(PER):
        t = i % 120; i += 1
        x, y = (X0 - 40 + t * 4, Y0 + t) if t < 60 else (X0 + 200 - (t - 60) * 4, Y0 + 60 - (t - 60))
        parts.append('A%d,%d_W%d' % (x, y, WAIT))
    t0 = time.time(); send('D_' + '_'.join(parts)); time.sleep(max(0, PER * WAIT / 1000.0 - (time.time() - t0)))
send('U')
