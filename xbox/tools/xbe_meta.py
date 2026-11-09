"""The XBE's dashboard metadata: title ID, title name and images.

  python xbox/tools/xbe_meta.py title-id <xbe>
      Sets the certificate's title ID to the Xbox 360 game's, 0x584108FF (the
      STFS header, the XEX execution info and the XDBF agree on it; see
      docs/package-analysis.md), so its saves have their own folder,
      E:\\UDATA\\584108FF. Run by tools/build.sh on every build (nxdk's cxbe
      has no switch for it).

  python xbox/tools/xbe_meta.py dashboard <xbe> <image.bin> <out folder> [<plain copy>]
      From the 360 game's own dashboard icon (its XDBF image 0x8000, a 64x64
      PNG in the XEX's resource, read from the decrypted image), writes
      TitleImage.xbx (128x128) and SaveImage.xbx (64x64), DXT1 textures in
      the XPR0 resources the Xbox dashboard reads, and TitleMeta.xbx, into
      the out folder (the game copies them to its save folder on start,
      src/xam.cpp), and inserts the title name and both images into the XBE
      as the sections an XDK build has ($$XTINFO, $$XTIMAGE, $$XSIMAGE; as
      Perfect Dark X's insert-xbe-dashboard-assets.py). The icon is the
      game's: it is made from the developer's copy of the game (tools/build.sh
      into build/release, tools/stage_disc.py), never kept in the repository.
      <plain copy>: the XBE as it was before, without the icon, written when
      the sections are added (the one the Windows installer ships, which must
      not carry the game's art).
"""
import hashlib
import io
import pathlib
import struct
import sys

TITLE_ID = 0x584108FF
TITLE_NAME = 'Geometry Wars Retro Evolved 2'
IMAGE_BASE = 0x82000000
XDBF_ADDRESS = 0x821FB480  # the XEX's XDBF resource (docs/package-analysis.md)
XDBF_SIZE = 0x17465
ICON_ID = 0x8000           # XDBF image namespace 2: the title's dashboard icon


def u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


# --- title ID -------------------------------------------------------------------------

def set_title_id(xbe):
    data = bytearray(xbe.read_bytes())
    if data[:4] != b'XBEH':
        sys.exit(f'{xbe} is not an XBE')
    base = u32(data, 0x104)
    at = u32(data, 0x118) - base + 8  # certificate + 8: title ID
    if not 0x178 <= at <= u32(data, 0x108) - 4:
        sys.exit(f'{xbe}: certificate outside the header')
    if u32(data, at) != TITLE_ID:
        struct.pack_into('<I', data, at, TITLE_ID)
        xbe.write_bytes(data)
    print(f'{xbe.name}: title ID {TITLE_ID:08X}')


# --- the icon ---------------------------------------------------------------------------

def game_icon(image_bin):
    """The XDBF's image 0x8000 (PNG bytes)."""
    image = image_bin.read_bytes()
    x = image[XDBF_ADDRESS - IMAGE_BASE:XDBF_ADDRESS - IMAGE_BASE + XDBF_SIZE]
    magic, _, table_len, count, free_len, _ = struct.unpack('>4sIIIII', x[:24])
    if magic != b'XDBF':
        sys.exit(f'{image_bin}: no XDBF resource at {XDBF_ADDRESS:08X}')
    data_at = 24 + table_len * 18 + free_len * 8
    for i in range(count):
        ns, rid, off, length = struct.unpack('>HQII', x[24 + i * 18:42 + i * 18])
        if ns == 2 and rid == ICON_ID:
            png = x[data_at + off:data_at + off + length]
            if png[:4] == b'\x89PNG':
                return png
    sys.exit(f'{image_bin}: the XDBF has no image {ICON_ID:X}')


def icon_pixels(png):
    """The icon's 64x64 RGBA pixels as stored (rows of bytes), the file's
    gamma not applied (installer/DashboardIcon.cs decodes them the same)."""
    from PIL import Image
    im = Image.open(io.BytesIO(png))
    if im.size != (64, 64):
        sys.exit(f'the icon is {im.size}, not 64x64')
    return im.convert('RGBA').tobytes()


def dxt1(rgb, size):
    """DXT1 blocks (opaque) for size x size RGB pixels (a list of (r, g, b)
    rows), all in whole numbers so the installer's copy of this
    (installer/DashboardIcon.cs) makes the same bytes: per block, the end
    colours are its darkest and brightest pixels (by 2R + 5G + B, the first
    found on a tie), and each pixel takes the nearest of the four colours
    (the lowest index on a tie)."""
    def pack(c):
        r, g, b = c
        return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255

    def unpack(v):
        r, g, b = v >> 11, (v >> 5) & 63, v & 31
        return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))

    out = bytearray()
    for by in range(0, size, 4):
        for bx in range(0, size, 4):
            px = [rgb[by + y][bx + x] for y in range(4) for x in range(4)]
            luma = [2 * r + 5 * g + b for r, g, b in px]
            lo, hi = luma.index(min(luma)), luma.index(max(luma))
            c0, c1 = pack(px[hi]), pack(px[lo])
            if c0 < c1:
                c0, c1 = c1, c0
            if c0 == c1:  # (four-colour mode needs c0 > c1; one colour: all index 0)
                out += struct.pack('<HHI', c0, c1, 0)
                continue
            e0, e1 = unpack(c0), unpack(c1)
            palette = [e0, e1, tuple((2 * a + b) // 3 for a, b in zip(e0, e1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(e0, e1))]
            bits = 0
            for k, p in enumerate(px):
                d = [sum((p[i] - q[i]) ** 2 for i in range(3)) for q in palette]
                bits |= d.index(min(d)) << (2 * k)
            out += struct.pack('<HHI', c0, c1, bits)
    return bytes(out)


def xpr0(rgba, size):
    """An XPR0 resource holding one size x size DXT1 texture (the layout the
    XDK's bundler writes for a title or save image: a 2048-byte header with
    one texture and the end marker, then the texture data), from the 64x64
    icon: laid on black (alpha), each pixel doubled for 128x128."""
    scale = size // 64
    rgb = []
    for y in range(size):
        row = []
        for x in range(size):
            at = ((y // scale) * 64 + x // scale) * 4
            r, g, b, a = rgba[at:at + 4]
            row.append(tuple((c * a + 127) // 255 for c in (r, g, b)))
        rgb.append(row)
    blocks = dxt1(rgb, size)
    log2 = size.bit_length() - 1
    # D3D format word: DMA channel A (1), 2D (bit 5), DXT1 (0x0C << 8), one
    # level (1 << 16), log2 width and height (bits 20 and 24).
    fmt = 0x29 | 0x0C << 8 | 1 << 16 | log2 << 20 | log2 << 24
    header = b'XPR0' + struct.pack('<II', 0x800 + len(blocks), 0x800)
    header += struct.pack('<IIIII', 0x00040001, 0, 0, fmt, 0) + b'\xff\xff\xff\xff'
    header += b'\xad' * (0x800 - len(header))
    return header + blocks


# --- the XBE's sections -----------------------------------------------------------------

def align(v, a):
    return (v + a - 1) & ~(a - 1)


def insert_sections(xbe, resources, plain=None):
    """Appends inserted-file sections (name, payload) to the XBE, the
    section table moved into the header's spare room (refused when it does
    not fit or the room is not empty). An XBE that has them already is left."""
    data = bytearray(xbe.read_bytes())
    base, header_size, image_size = u32(data, 0x104), u32(data, 0x108), u32(data, 0x10C)
    count, table = u32(data, 0x11C), u32(data, 0x120) - base
    names = []
    for i in range(count):
        name_at = u32(data, table + i * 56 + 20) - base
        names.append(bytes(data[name_at:data.index(b'\0', name_at)]).decode())
    if any(n in names for n, _ in resources):
        print(f'{xbe.name}: dashboard sections already there')
        return
    if plain:
        plain.write_bytes(data)
    old = bytes(data[table:table + count * 56])
    first_raw = min(u32(old, i * 56 + 12) for i in range(count) if u32(old, i * 56 + 12))
    new_table = align(header_size, 16)
    cursor = new_table + (count + len(resources)) * 56
    name_offsets = []
    for name, _ in resources:
        name_offsets.append(cursor)
        cursor += len(name) + 1
    refs = align(cursor, 2)
    new_header = align(refs + (len(resources) + 1) * 2, 4)
    if new_header > first_raw:
        sys.exit(f'{xbe}: no room in the header for the dashboard sections')
    if any(data[new_table:new_header]):
        sys.exit(f'{xbe}: the header\'s spare room is not empty')
    data[new_table:new_table + len(old)] = old
    for at, (name, _) in zip(name_offsets, resources):
        data[at:at + len(name) + 1] = name.encode() + b'\0'
    raw = align(len(data), 0x1000)
    data += b'\0' * (raw - len(data))
    virtual = base + align(image_size, 0x1000)
    for i, ((_, payload), name_at) in enumerate(zip(resources, name_offsets)):
        struct.pack_into('<9I20s', data, new_table + (count + i) * 56, 0x38, virtual, len(payload), len(data),
                         len(payload), base + name_at, 0, base + refs + i * 2, base + refs + (i + 1) * 2,
                         hashlib.sha1(payload).digest())
        data += payload
        data += b'\0' * (align(len(data), 0x1000) - len(data))
        virtual = align(virtual + len(payload), 0x20)
    struct.pack_into('<I', data, 0x108, new_header)
    struct.pack_into('<I', data, 0x10C, virtual - base)
    struct.pack_into('<I', data, 0x11C, count + len(resources))
    struct.pack_into('<I', data, 0x120, base + new_table)
    xbe.write_bytes(data)
    print(f'{xbe.name}: dashboard sections inserted')


def dashboard(xbe, image_bin, out, plain=None):
    png = game_icon(image_bin)
    rgba = icon_pixels(png)
    title_image, save_image = xpr0(rgba, 128), xpr0(rgba, 64)
    meta = f'[default]\r\nTitleName={TITLE_NAME}\r\n'
    out.mkdir(parents=True, exist_ok=True)
    for name, data in (('TitleImage.xbx', title_image), ('SaveImage.xbx', save_image),
                       ('TitleMeta.xbx', meta.encode('utf-16'))):
        path = out / name
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    insert_sections(xbe, [('$$XTINFO', meta.encode('ascii')), ('$$XTIMAGE', title_image),
                          ('$$XSIMAGE', save_image)], plain)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'title-id':
        set_title_id(pathlib.Path(sys.argv[2]))
    elif len(sys.argv) in (5, 6) and sys.argv[1] == 'dashboard':
        dashboard(pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4]),
                  pathlib.Path(sys.argv[5]) if len(sys.argv) == 6 else None)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
