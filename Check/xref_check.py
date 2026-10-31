import struct, capstone

path = r'C:\Temp\wb_verify\l4d2_payload.dll'
data = open(path, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
secoff = pe + 24 + optsz
imgbase = struct.unpack_from('<I', data, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = secoff + i * 40
    name = data[o:o + 8].rstrip(b'\0').decode('latin1')
    vsz, va, rsz, ro = struct.unpack_from('<IIII', data, o + 8)
    secs.append((name, va, vsz, ro, rsz))

def off2va(off):
    for name, va, vsz, ro, rsz in secs:
        if ro <= off < ro + rsz:
            return imgbase + va + (off - ro)
    return None

# 恶意字符串 VA
str_va = []
for kw in (b'devruntime', b'BK377375'):
    off = data.find(kw)
    while off != -1:
        va = off2va(off)
        str_va.append((kw.decode(), va, off))
        off = data.find(kw, off + 1)
print("恶意字符串 VA:", [(k, hex(v)) for k, v, _ in str_va])

# 在 .text 中找对这些 VA 的引用（push imm32 / mov reg, imm32）
text = [s for s in secs if s[0] == '.text'][0]
_, tva, tvsz, tro, trsz = text
code = data[tro:tro + trsz]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
targets = {v for _, v, _ in str_va if v}
found = []
for ins in md.disasm(code, imgbase + tva):
    if ins.mnemonic in ('push', 'mov', 'lea') and '0x' in ins.op_str:
        try:
            imm = int(ins.op_str.split(',')[-1].strip(), 16)
        except ValueError:
            continue
        if imm in targets:
            found.append((ins.address, ins.mnemonic, ins.op_str))
print(f".text 中对恶意字符串的引用: {len(found)} 处")
for a, m, o in found[:10]:
    print(f"  0x{a:08X}: {m} {o}")

# 入口点附近反汇编
ep = struct.unpack_from('<I', data, pe + 24 + 16)[0]
print(f"\n=== 入口点 0x{ep:X} 附近 ===")
epoff = None
for name, va, vsz, ro, rsz in secs:
    if va <= ep < va + max(vsz, rsz):
        epoff = ro + (ep - va)
        break
for ins in md.disasm(data[epoff:epoff + 64], imgbase + ep):
    print(f"  {ins.address:08X}: {ins.mnemonic:8s} {ins.op_str}")
