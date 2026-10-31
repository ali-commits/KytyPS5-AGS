"""Prepare the selected v1.018 profile without overwriting import metadata."""
from pathlib import Path
import copy, hashlib, json, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / 'source-profile.json'
OLD, NEW, SIZE = 0xf4e5440, 0x12004000, 0x88
SELECTED = {2, 3, 4, 5, 9, 13, 14, 15, 16, 17, 18, 20, 26, 27}
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
source = json.loads(SOURCE.read_text())
profile = copy.deepcopy(source)
profile['name'] = 'ASTRO BOT v1.018 test: 1080p, 60 FPS cap, timing/physics fixes; separate patch state'
changes = []
for index, mod in enumerate(profile['mods']):
    mod['enabled'] = index in SELECTED
    for write in mod['memory']:
        address = int(write['offset'], 0)
        if OLD <= address < OLD + SIZE:
            assert address + len(bytes.fromhex(write['on'])) <= OLD + SIZE
            write['offset'] = hex(NEW + address - OLD)
            changes.append({'mod': index, 'kind': 'state-write', 'old': hex(address), 'new': write['offset']})
            continue
        blob = bytearray.fromhex(write['on'])
        for ins in md.disasm(bytes(blob), address):
            for operand in ins.operands:
                if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                    continue
                target = ins.address + ins.size + operand.mem.disp
                if not OLD <= target < OLD + SIZE:
                    continue
                # RIP-relative x86-64 addressing uses disp32. Capstone 5.0.9
                # misreports disp_size=2 for this VEX vucomisd; verify raw bytes.
                assert ins.disp_offset > 0 and ins.disp_offset + 4 <= ins.size
                assert struct.unpack_from('<i', ins.bytes, ins.disp_offset)[0] == operand.mem.disp
                new_target = NEW + target - OLD
                pos = ins.address - address + ins.disp_offset
                blob[pos:pos + 4] = struct.pack('<i', new_target - ins.address - ins.size)
                check = next(md.disasm(bytes(blob[ins.address-address:ins.address-address+ins.size]), ins.address))
                assert check.size == ins.size and check.mnemonic == ins.mnemonic
                assert check.address + check.size + check.disp == new_target
                changes.append({'mod': index, 'kind': 'rip-reference', 'instruction': hex(ins.address),
                                'old': hex(target), 'new': hex(new_target)})
        write['on'] = blob.hex()
assert sum(c['kind'] == 'rip-reference' for c in changes) >= 20
output = ROOT / 'profile.json'
output.write_text(json.dumps(profile, indent=2) + '\n')
report = {'source': str(SOURCE), 'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
          'profile': str(output), 'profile_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
          'state_old': hex(OLD), 'state_new': hex(NEW), 'state_bytes': SIZE,
          'enabled_mods': [m['name'] for m in profile['mods'] if m['enabled']],
          'relocations': changes, 'runtime_tested': False}
# The generated profile is tracked; runtime provenance belongs in the local build directory.
print(json.dumps({'enabled_mods': len(report['enabled_mods']), 'state_relocations': len(changes),
                  'profile': str(output), 'sha256': report['profile_sha256']}))
