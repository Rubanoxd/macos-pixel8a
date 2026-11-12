#!/usr/bin/env python3
"""Live control of the Pixel 8a (macOS) over the USB cable, without a camera.

Talks UDP to the AkitaUsbGadget.h gadget (boot-arg akita-usb-gadget=2), on the interface with MAC 02:a5:1a:00:00:01:
  log  [file]              the live kernel log (UDP 5140, broadcast), also saved to the file
  replay                   asks the phone to resend the log from the start (then use `log`)
  steps "<steps>"          input steps in akita-input-script syntax (K28, T..., A x,y, C, P x,y, M, W ms)
  type "text"              types text (Spanish layout; '_' and '%' are encoded automatically)
  run "command"            types the command and presses Return (in the focused Terminal)
  key <usage>[,<mods>] ... keys by HID usage in hex (28 Return, 2b Tab, 29 Esc; mods 08 = Cmd, 02 = Shift)
  click x y                clicks at (x, y) with the absolute pointer (display mode coordinates)
  shot [out.png] [n]       screenshot scaled down n times (default 2)
"""
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

PHONE = '169.254.7.2'
MAC = '02:a5:1a:00:00:01'
OUT = '/Volumes/datos2/unixnu-akita/scratch/live'


def interface():
    for name in subprocess.run(['ifconfig', '-l'], capture_output=True, text=True).stdout.split():
        info = subprocess.run(['ifconfig', name], capture_output=True, text=True).stdout
        if 'ether ' + MAC in info:
            return name
    sys.exit('can't find the Pixel's USB interface (ether %s)' % MAC)


def udp_socket(bind_port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    index = socket.if_nametoindex(interface())
    s.setsockopt(socket.IPPROTO_IP, 25, index)          # IP_BOUND_IF: always go out through the USB cable
    s.bind(('0.0.0.0', bind_port))
    return s


def send(command):
    s = udp_socket()
    s.sendto(command.encode(), (PHONE, 5141))
    return s


def encode_text(text):
    out = []
    for ch in text:
        if ch == ' ':
            out.append('+')
        elif ch.isalnum() or ch in '.,;:-/!"$&()=\'?*|@#':
            out.append(ch)
        else:
            out.append('%%%02x' % ord(ch))
    return 'T' + ''.join(out) + '_'


def png(path, width, height, rgb):
    raw = b''.join(b'\x00' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def shot(path, scale):
    s = send('S%d' % scale)
    s.settimeout(3)
    image, got, total, size, frame = None, 0, None, None, None
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            data = s.recv(2048)
        except socket.timeout:
            break
        if data[:4] != b'AKFB':
            continue
        fid, offset, tot = struct.unpack_from('<III', data, 4)
        w, h = struct.unpack_from('<HH', data, 16)
        if frame is None:
            frame, total, size = fid, tot, (w, h)
            image = bytearray(tot)
        if fid != frame:
            continue
        payload = data[20:]
        image[offset:offset + len(payload)] = payload
        got += len(payload)
        if got >= total:
            break
    if image is None:
        sys.exit('no answer from the phone')
    png(path, size[0], size[1], bytes(image))
    print('%s: %dx%d, %d/%d bytes%s' % (path, size[0], size[1], got, total, '' if got >= total else ' (INCOMPLETA)'))


def log(path):
    s = udp_socket(5140)
    expected = None
    with open(path, 'ab') as f:
        while True:
            data = s.recv(2048)
            if data[:4] != b'AKLG':
                continue
            offset = struct.unpack_from('<I', data, 4)[0]
            text = data[8:]
            if expected is not None and offset != expected:
                gap = (offset - expected) & 0xffffffff
                note = (b'\n[... log from the start ...]\n' if gap >= 0x80000000 else
                        ('\n[... %d bytes perdidos ...]\n' % gap).encode())
                sys.stdout.buffer.write(note)
                f.write(note)
            expected = (offset + len(text)) & 0xffffffff
            sys.stdout.buffer.write(text)
            sys.stdout.flush()
            f.write(text)
            f.flush()


def main():
    os.makedirs(OUT, exist_ok=True)
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    what = args[0]
    if what == 'log':
        log(args[1] if len(args) > 1 else os.path.join(OUT, 'kernel-%s.log' % time.strftime('%H%M%S')))
    elif what == 'replay':
        send('L')
    elif what == 'steps':
        send('I' + args[1])
    elif what == 'type':
        send('I' + encode_text(args[1]))
    elif what == 'run':
        send('I' + encode_text(args[1]) + 'K28')
    elif what == 'key':
        send('I' + '_'.join('K' + k for k in args[1:]))
    elif what == 'click':
        send('IA%s,%s_C' % (args[1], args[2]))
    elif what == 'shot':
        path = args[1] if len(args) > 1 else os.path.join(OUT, 'shot-%s.png' % time.strftime('%H%M%S'))
        shot(path, int(args[2]) if len(args) > 2 else 2)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
