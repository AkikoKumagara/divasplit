"""Minimal Vita ELF (decrypted eboot.bin) reader and Thumb-2 helpers shared by
mkpatterns.py and port.py."""
import re
import struct

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs


class VitaElf:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        if d[:4] != b'\x7fELF':
            raise ValueError(f'{path}: not an ELF (decrypt the eboot first, e.g. with FAGDec)')
        phoff = struct.unpack_from('<I', d, 0x1C)[0]
        phnum = struct.unpack_from('<H', d, 0x2C)[0]
        self.segs = []
        for i in range(phnum):
            p_type, p_off, p_va, _, p_fsz, p_msz, _, _ = struct.unpack_from('<8I', d, phoff + i * 32)
            self.segs.append((p_type, p_off, p_va, p_fsz, p_msz))
        _, self.text_off, self.text_va, self.text_size, _ = self.segs[0]
        self.text = d[self.text_off:self.text_off + self.text_size]
        self.seg1_va = self.segs[1][2]

        entry = struct.unpack_from('<I', d, 0x18)[0]
        mi = self.segs[entry >> 30][1] + (entry & 0x3FFFFFFF)
        self.module_name = d[mi + 4:mi + 31].split(b'\0')[0].decode('latin1')
        self.module_nid = struct.unpack_from('<I', d, mi + 32 + 20)[0]
        self._starts = None

    def u16(self, off):
        return struct.unpack_from('<H', self.text, off)[0]

    def insn_starts(self):
        """Instruction start offsets in seg0 from a linear Thumb sweep."""
        if self._starts is None:
            md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
            starts = bytearray(len(self.text))
            off = 0
            n = len(self.text)
            while off < n:
                progressed = False
                for addr, size, _, _ in md.disasm_lite(self.text[off:off + 0x10000], off):
                    starts[addr] = size
                    off = addr + size
                    progressed = True
                if not progressed:
                    off += 2
            self._starts = starts
        return self._starts

    def insn_at(self, off):
        """(offset, size) list of instructions around off is built by callers."""
        return self.insn_starts()[off]


def insn_mask(code, keep_imm=False):
    """Mask for one Thumb instruction: bits that may differ between builds of
    the same code (branch / call targets, PC-relative literals, and unless
    keep_imm the immediates of movw/movt, which usually load addresses)."""
    if len(code) == 2:
        hw = struct.unpack('<H', code)[0]
        m = 0xFFFF
        if (hw & 0xF800) == 0xE000:            # b
            m = 0xF800
        elif (hw & 0xF000) == 0xD000 and (hw & 0x0E00) != 0x0E00:   # bcc
            m = 0xFF00
        elif (hw & 0xF500) == 0xB100:          # cbz / cbnz
            m = 0xFD07
        elif (hw & 0xF800) in (0x4800, 0xA000):    # ldr literal, adr
            m = 0xFF00
        return struct.pack('<H', m)
    hw1, hw2 = struct.unpack('<HH', code)
    m1, m2 = 0xFFFF, 0xFFFF
    if (hw1 & 0xF800) == 0xF000 and (hw2 & 0x8000) == 0x8000:    # bl, blx, b.w, bcc.w
        m1, m2 = 0xF800, 0xD000
    elif (hw1 & 0xFBF0) in (0xF240, 0xF2C0) and (hw2 & 0x8000) == 0:   # movw / movt
        if not keep_imm:
            m1, m2 = 0xFBF0, 0x0F00
    elif (hw1 & 0xFF7F) == 0xF85F:            # ldr.w literal
        m1, m2 = 0xFF7F, 0xF000
    elif (hw1 & 0xFF3F) == 0xED1F:            # vldr literal
        m1, m2 = 0xFF7F, 0xFF00
    elif (hw1 & 0xFBFF) in (0xF20F, 0xF2AF):  # adr.w
        m1, m2 = 0xFBFF, 0x8F00
    return struct.pack('<HH', m1, m2)


def pattern_regex(values, masks):
    out = []
    for v, m in zip(values, masks):
        if m == 0xFF:
            out.append(re.escape(bytes([v])))
        elif m == 0:
            out.append(b'.')
        else:
            ok = [b for b in range(256) if (b & m) == (v & m)]
            out.append(b'[' + b''.join(re.escape(bytes([b])) for b in ok) + b']')
    return re.compile(b'(?=(' + b''.join(out) + b'))', re.DOTALL)


def find_all(text, values, masks):
    rx = pattern_regex(values, masks)
    return [m.start() for m in rx.finditer(text) if m.start() % 2 == 0]


def decode_movw_movt(code8):
    """Value loaded by a movw + movt pair (8 bytes)."""
    def imm16(hw1, hw2):
        return ((hw1 & 0xF) << 12) | (((hw1 >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) | (hw2 & 0xFF)
    a1, a2, b1, b2 = struct.unpack('<HHHH', code8)
    return imm16(a1, a2) | (imm16(b1, b2) << 16)


def decode_ldrb_imm(code):
    """Offset of ldrb rt, [rn, #imm] (16-bit T1 or 32-bit T2)."""
    if len(code) == 2 or (struct.unpack_from('<H', code)[0] & 0xF800) == 0x7800:
        hw = struct.unpack_from('<H', code)[0]
        return (hw >> 6) & 0x1F
    hw1, hw2 = struct.unpack('<HH', code[:4])
    return hw2 & 0xFFF
