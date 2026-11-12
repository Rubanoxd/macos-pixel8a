#!/usr/bin/env python3
"""Control en vivo del Pixel 8a (macOS Recovery) por el cable USB, sin cámara.

Habla con el gadget de AkitaUsbGadget.h (boot-arg akita-usb-gadget=2) por UDP, en la interfaz con MAC 02:a5:1a:00:00:01:
  log  [fichero]           el log del kernel en vivo (UDP 5140, broadcast), también guardado en el fichero
  replay                   pide al teléfono que reenvíe el log desde el principio (y luego usa `log`)
  steps "<pasos>"          pasos de entrada con la sintaxis de akita-input-script (K28, T..., A x,y, C, P x,y, M, W ms)
  type "texto"             escribe texto (teclado español; '_' y '%' se codifican solos)
  run "orden"              escribe la orden y pulsa Return (en el Terminal que tenga el foco)
  key <uso>[,<mods>] ...   teclas por uso HID en hex (28 Return, 2b Tab, 29 Esc; mods 08 = Cmd, 02 = Shift)
  click x y                toca la pantalla en (x, y) con el puntero absoluto (coordenadas del modo, 540x1200)
  shot [salida.png] [n]    captura de pantalla reducida n veces (2 por defecto: 540x1200)
  fastboot                 reinicia el teléfono directo a fastboot (lo hace el kernel; sirve aunque el shell no responda)

Por Wi-Fi (canal de AkitaPcie.inc, para cuando el USB-C tiene un dongle; el Mac en la misma red):
  wlog [fichero]           saluda por broadcast (UDP 5141) y recibe el log del kernel por unicast; guarda la IP del teléfono
                           en scratch/live/phone-wifi-ip (para `AKHOST=$(cat ...) scratch/akx ...`)
  wfastboot                como `fastboot`, a la IP guardada (solo lo acepta del Mac que saludó)
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
    sys.exit('no encuentro la interfaz USB del Pixel (ether %s)' % MAC)


def udp_socket(bind_port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    index = socket.if_nametoindex(interface())
    s.setsockopt(socket.IPPROTO_IP, 25, index)          # IP_BOUND_IF: salir siempre por el cable USB
    s.bind(('0.0.0.0', bind_port))
    return s


def send(command):
    s = udp_socket()
    s.sendto(command.encode(), (PHONE, 5141))
    return s


WIFI_PORT = 5143
WIFI_IP_FILE = os.path.join(OUT, 'phone-wifi-ip')


def wifi_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(('0.0.0.0', WIFI_PORT))
    return s


def wsend(command):
    try:
        phone = open(WIFI_IP_FILE).read().strip()
    except OSError:
        phone = '255.255.255.255'   # sin IP todavía: por broadcast (el teléfono solo lo acepta del Mac que saludó)
    wifi_socket().sendto(command.encode(), (phone, 5141))


def wlog(path):
    s = wifi_socket()
    s.settimeout(1.0)
    expected, phone, last_hello = None, None, 0.0
    with open(path, 'ab') as f:
        while True:
            if time.time() - last_hello > 3:   # hello: registers this Mac (and keeps its address fresh)
                for to in ['255.255.255.255'] + ([phone] if phone else []):
                    try:
                        s.sendto(b'H', (to, 5141))
                    except OSError:
                        pass   # no route, host down: the phone may be rebooting
                last_hello = time.time()
            try:
                data, addr = s.recvfrom(2048)
            except (socket.timeout, OSError):
                continue
            if data[:4] != b'AKLG':
                continue
            if addr[0] != phone and addr[0] != '0.0.0.0':
                phone = addr[0]
                with open(WIFI_IP_FILE, 'w') as g:
                    g.write(phone + '\n')
            offset = struct.unpack_from('<I', data, 4)[0]
            text = data[8:]
            if expected is not None and offset != expected:
                gap = (offset - expected) & 0xffffffff
                note = (b'\n[... log desde el principio ...]\n' if gap >= 0x80000000 else
                        ('\n[... %d bytes perdidos ...]\n' % gap).encode())
                sys.stdout.buffer.write(note)
                f.write(note)
            expected = (offset + len(text)) & 0xffffffff
            sys.stdout.buffer.write(text)
            sys.stdout.flush()
            f.write(text)
            f.flush()


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
        sys.exit('sin respuesta del teléfono')
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
                note = (b'\n[... log desde el principio ...]\n' if gap >= 0x80000000 else
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
    elif what == 'wlog':
        wlog(args[1] if len(args) > 1 else os.path.join(OUT, 'wifi-%s.log' % time.strftime('%H%M%S')))
    elif what == 'wfastboot':
        wsend('FASTBOOT')
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
    elif what == 'fastboot':
        send('FASTBOOT')
    elif what == 'shot':
        path = args[1] if len(args) > 1 else os.path.join(OUT, 'shot-%s.png' % time.strftime('%H%M%S'))
        shot(path, int(args[2]) if len(args) > 2 else 2)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
