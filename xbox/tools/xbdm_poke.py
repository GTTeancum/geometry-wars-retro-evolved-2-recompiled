"""Test only: writes 32-bit words into a symbol of the running game (from the
link map, xbox/build/gw2.map) through xbdm. For main memory only (xbdm's
setmem writes byte by byte: never point it at hardware registers).
  python xbox/tools/xbdm_poke.py <console ip> <symbol> <word> [word ...]
Words in hex, e.g. gw_draw_skip 6FA9F218 0 (make DRAW_SKIP=y).
"""
import pathlib
import socket
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import xbdm_log  # noqa: E402

host, name, words = sys.argv[1], sys.argv[2], [int(w, 16) for w in sys.argv[3:]]
text = (pathlib.Path(__file__).resolve().parents[1] / 'build' / 'gw2.map').read_text(errors='replace')
address = xbdm_log.symbol(text, name)
sock = socket.create_connection((host, 731), timeout=10)
sock.recv(256)
data = b''.join(struct.pack('<I', w) for w in words)
sock.sendall(f'setmem addr=0x{address:08X} data={data.hex()}\r\n'.encode())
print(sock.recv(256).decode(errors='replace').strip())
sock.sendall(b'bye\r\n')
