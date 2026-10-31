import struct

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

# 找恶意命令串的起始位置（往前找行首/字符串起点）
off = data.find(b'devruntime')
start = data.rfind(b'\x00', max(0, off - 300), off) + 1
cmd = data[start:data.find(b'\x00', off)]
print(f"恶意命令串起点 VA=0x{off2va(start):X} 长度={len(cmd)}")
print(f"  内容: {cmd[:120]!r}")

# 在 .text 里找对该簇任意 VA 的 4 字节立即数引用
cluster_lo = off2va(start) - 0x40
cluster_hi = off2va(start) + 0x200
_, tva, tvsz, tro, trsz = [s for s in secs if s[0] == '.text'][0]
code = data[tro:tro + trsz]
refs = []
for i in range(0, len(code) - 4):
    imm = struct.unpack_from('<I', code, i)[0]
    if cluster_lo <= imm <= cluster_hi:
        refs.append((imgbase + tva + i, imm))
print(f".text 中引用该字符串簇的立即数: {len(refs)} 处")
for a, imm in refs[:12]:
    ctx = code[max(0, (a - imgbase - tva) - 8):(a - imgbase - tva) + 8]
    print(f"  代码 0x{a:08X} -> 字符串 0x{imm:08X}  前后字节 {ctx.hex()}")
