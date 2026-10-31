"""Read-only preflight for the pinned Astro Bot v1.018 test profile."""
from pathlib import Path
import hashlib, json, struct
ROOT = Path(__file__).resolve().parent
PATCH = ROOT / 'profile.json'
GAME_HASH = '3f6873c9aa0262b3e5513201810a4c40e0dd5ee910567a3a06fcc412275a1e16'
PATCH_HASH = 'dd88b91e85f82a9a768e1e0732d5c7c3e93c8055d00754314ef4049c635b3f5b'
SELECTED = {2,3,4,5,9,13,14,15,16,17,18,20,26,27}

def require(condition, message):
    if not condition:
        raise ValueError(message)


def image(data):
    require(data[:4] == bytes.fromhex('4f153d1d'), 'Unexpected executable container')
    count = struct.unpack_from('<H', data, 24)[0]
    elf = 32 + count * 32
    h = struct.unpack_from('<16sHHIQQQIHHHHHH', data, elf)
    require(h[0][:5] == b'\x7fELF\x02' and h[9] == 56, 'Unexpected ELF header')
    ph = [struct.unpack_from('<IIQQQQQQ', data, elf+h[5]+i*h[9]) for i in range(h[10])]
    maps = {}
    for i in range(count):
        typ, off, packed, size = struct.unpack_from('<QQQQ', data, 32+i*32)
        if typ & 0x800:
            maps[(typ >> 20) & 0xfff] = (off, packed, size)
    loads = [(i,s) for i,s in enumerate(ph) if s[0] in (1, 0x61000010) and s[6]]
    end = max(s[3]+s[6] for _,s in loads)
    def read(address, length):
        result = bytearray()
        for a in range(address, address+length):
            byte = None
            for i,s in loads:
                if s[3] <= a < s[3]+s[5]:
                    off,packed,size = maps[i]
                    require(packed == size == s[5], 'Compressed SELF segment unsupported')
                    byte = data[off+a-s[3]]
                    break
                if s[3]+s[5] <= a < s[3]+s[6]:
                    byte = 0
                    break
            if byte is None:
                # Allocated program gaps are zero; external patch caves are allocated separately.
                byte = 0
            result.append(byte)
        return bytes(result)
    return read, end

def check_writes(profile, read, end):
    writes = []
    overlaps = []
    for index, mod in enumerate(profile['mods']):
        if not mod['enabled']:
            continue
        for w in mod['memory']:
            a = int(w['offset'], 0)
            off, on = bytes.fromhex(w['off']), bytes.fromhex(w['on'])
            require(len(off) == len(on) and bool(off), 'Invalid write size')
            require(read(a, len(off)) == off, f'Original bytes mismatch: mod {index}, {a:#x}')
            if a >= end:
                require(not any(off) and 0x12000000 <= a and a+len(off) <= 0x12008000, 'Unexpected cave')
            for old_index, start, stop in writes:
                if a < stop and start < a+len(on):
                    require((old_index,index,a,len(on)) == (2,20,0x74bce84,8), 'Unexpected overlapping edits')
                    overlaps.append({'first_mod':old_index,'second_mod':index,'address':hex(a),'bytes':len(on)})
            writes.append((index,a,a+len(on)))
    return {'writes_verified':len(writes),'intentional_overlaps':overlaps}

def validate(game, patch=PATCH):
    game, patch = Path(game), Path(patch)
    param = json.loads((game/'sce_sys/param.json').read_text())
    require((param['titleId'],param['contentVersion']) == ('PPSA21567','01.018.000'), 'Wrong game version')
    data = (game/'eboot.bin').read_bytes()
    require(hashlib.sha256(data).hexdigest() == GAME_HASH, 'Executable hash changed')
    raw = patch.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == PATCH_HASH, 'Patch profile changed; revalidate before launching')
    profile = json.loads(raw)
    require({i for i,m in enumerate(profile['mods']) if m['enabled']} == SELECTED, 'Unexpected enabled mods')
    require(profile['id'] == param['titleId'] and profile['version'] == param['contentVersion'], 'Profile metadata mismatch')
    read,end = image(data)
    result = check_writes(profile,read,end)
    result.update(game=str(game), patch=str(patch), game_sha256=GAME_HASH, patch_sha256=PATCH_HASH,
                  expected_resolution=[1920,1080], expected_cap=60, selected_mods=len(SELECTED))
    return result

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--patch', type=Path, default=PATCH)
    args = parser.parse_args()
    print(json.dumps(validate(args.game, args.patch), indent=2))
