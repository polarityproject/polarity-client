# Lists every file path in the game's pak indexes (UE4 pak version 3, not
# encrypted), one per line as "<pak>\t/Game/...". For finding assets to load
# by path (showtex, LoadAsset) without unpacking anything.
#   python -I scripts/pak-paths.py [folder of .pak files] > paths.txt
import glob
import os
import struct
import sys

MAGIC = 0x5A6F12E1
PAKS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'StreetFighterV', 'Content', 'Paks')


def fstring(b, o):
    n = struct.unpack_from('<i', b, o)[0]
    o += 4
    if n < 0:
        return b[o:o - 2 * n].decode('utf-16-le').rstrip('\0'), o - 2 * n
    return b[o:o + n].decode('utf-8', 'replace').rstrip('\0'), o + n


def skip_entry(b, o, version):
    o += 24  # offset, size, uncompressed size
    method = struct.unpack_from('<i', b, o)[0]
    o += 4
    if version <= 1:
        o += 8  # timestamp
    o += 20  # sha1
    if version >= 3:
        if method != 0:
            o += 4 + 16 * struct.unpack_from('<i', b, o)[0]  # compression blocks
        o += 5  # encrypted flag, block size
    return o


def game_path(mount, name):
    # "../../../StreetFighterV/Content/Chara/RYU/Hud/X.uasset" -> "/Game/Chara/RYU/Hud/X"
    path = (mount + name).replace('../../../StreetFighterV/Content', '/Game', 1)
    return path[:-len('.uasset')] if path.endswith('.uasset') else path


def main():
    out = sys.stdout
    for pak in sorted(glob.glob(os.path.join(sys.argv[1] if len(sys.argv) > 1 else PAKS, '*.pak'))):
        with open(pak, 'rb') as f:
            f.seek(-44, 2)
            magic, version, offset, size = struct.unpack_from('<IiQQ', f.read(44))
            if magic != MAGIC:
                print(f'{os.path.basename(pak)}: no pak footer', file=sys.stderr)
                continue
            f.seek(offset)
            index = f.read(size)
        mount, o = fstring(index, 0)
        count = struct.unpack_from('<i', index, o)[0]
        o += 4
        for _ in range(count):
            name, o = fstring(index, o)
            o = skip_entry(index, o, version)
            out.write(f'{os.path.basename(pak)}\t{game_path(mount, name)}\n')


if __name__ == '__main__':
    main()
