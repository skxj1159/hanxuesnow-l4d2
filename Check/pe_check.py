import struct, sys

def pe_info(path):
    data = open(path, 'rb').read()
    print(f"\n=== {path}  ({len(data)} bytes)")
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    secoff = pe + 24 + optsz
    ep = struct.unpack_from('<I', data, pe + 24 + 16)[0]
    imgbase = struct.unpack_from('<I', data, pe + 24 + 28)[0]
    print(f"  ImageBase=0x{imgbase:X}  AddressOfEntryPoint=0x{ep:X}  sections={nsec}")
    last_end = 0
    for i in range(nsec):
        o = secoff + i * 40
        name = data[o:o + 8].rstrip(b'\0').decode('latin1')
        vsz, va, rsz, ro = struct.unpack_from('<IIII', data, o + 8)
        flags = struct.unpack_from('<I', data, o + 36)[0]
        last_end = max(last_end, ro + rsz)
        print(f"  [{name:8s}] VA=0x{va:06X} raw=0x{ro:06X}+0x{rsz:X} flags=0x{flags:08X}")
    print(f"  最后节结束=0x{last_end:X}  文件大小=0x{len(data):X}  尾部余量={len(data)-last_end} bytes")
    # 找恶意字符串的文件偏移与所属节
    for kw in (b'devruntime', b'BK377375'):
        off = data.find(kw)
        while off != -1:
            sec = '?'
            for i in range(nsec):
                o = secoff + i * 40
                name = data[o:o + 8].rstrip(b'\0').decode('latin1')
                vsz, va, rsz, ro = struct.unpack_from('<IIII', data, o + 8)
                if ro <= off < ro + rsz:
                    sec = name
                    break
            else:
                if off >= last_end:
                    sec = 'OVERLAY(节表外!)'
            ctx = data[max(0, off - 24):off + 48]
            print(f"  字符串 {kw.decode()} @ 0x{off:X} 节={sec}  上下文={ctx!r}")
            off = data.find(kw, off + 1)

pe_info(r'C:\Temp\wb_verify\l4d2_payload.dll')
pe_info(r'C:\Users\Administrator\Desktop\求生之路2项目\build\hanxuesnow.exe')
