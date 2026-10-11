# SPDX-License-Identifier: GPL-3.0-or-later
"""Arithmetic decoder for IFX compressed bitstreams (Shockwave 3D, same scheme as ECMA-363 U3D).

Python port of vm-rust/src/director/chunks/w3d/bitstream.rs from dirplayer-rs
(https://github.com/igorlira/dirplayer-rs, GPL-3.0).

Contexts: 0 = raw bytes, 1..0x400 = adaptive histograms, above 0x400 = uniform
distribution over (context - 0x400) symbols, >= MAX_RANGE = raw.
"""

STATIC_FULL = 0x400
MAX_RANGE = STATIC_FULL + 0x3FFF
HALF = 0x8000
QUARTER = 0x4000
ELEPHANT = 0x1FFF
MAX_HIST_SYMBOL = 0xFFFF
ARRAY_INCR = 32
M32 = 0xFFFFFFFF

SWAP8 = (0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)
FAST_NOT_MASK = (0xFFFF, 0x7FFF, 0x3FFF, 0x1FFF, 0x0FFF)
READ_COUNT = (4, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0)


class BitStream:
    def __init__(self, block):
        pad = (-len(block)) % 4
        raw = block + b"\0" * (pad + 64)
        self.data = memoryview(raw).cast("I").tolist()
        self.high = 0xFFFF
        self.low = 0
        self.underflow = 0
        self.pos = 0  # bit position
        self.sym = {}  # context -> per-symbol counts
        self.cum = {}  # context -> counts of symbols >= index (cum[0] is the total)

    # --- raw reads ---

    def _word(self, i):
        d = self.data
        return d[i] if i < len(d) else 0

    def _bits(self, n):
        """Peek n (<= 32) bits at the current position, LSB first."""
        i, off = self.pos >> 5, self.pos & 31
        v = self._word(i) >> off
        if off + n > 32:
            v |= self._word(i + 1) << (32 - off)
        return v & ((1 << n) - 1)

    def u8(self):
        if self.high == 0xFFFF and self.low == 0 and self.underflow == 0:
            v = self._bits(8)
            self.pos += 8
            return v
        v = self._symbol_static(0x500)
        return (SWAP8[v & 0xF] << 4) | SWAP8[(v >> 4) & 0xF]

    def u16(self):
        lo = self.u8()
        return lo | (self.u8() << 8)

    def u32(self):
        lo = self.u16()
        return lo | (self.u16() << 16)

    def string(self):
        return "".join(chr(self.u8()) for _ in range(self.u16()))

    # --- compressed reads ---

    def _compressed(self, context, raw):
        if context == 0 or context >= MAX_RANGE:
            return raw()
        symbol = self._symbol_static(context) if context > STATIC_FULL else self._symbol_dynamic(context)
        if symbol:
            return symbol - 1
        val = raw()  # escape
        if context < STATIC_FULL:
            self._add_symbol(context, val + 1)
        return val

    def cu32(self, context):
        return self._compressed(context, self.u32)

    def cu16(self, context):
        return self._compressed(context, self.u16)

    def cu8(self, context):
        return self._compressed(context, self.u8)

    # --- arithmetic decoder core ---

    def _read_code(self):
        save = self.pos
        code = self._bits(1)
        self.pos += 1 + self.underflow
        v = (self._bits(15) << 1) & 0xFFFF
        self.pos = save
        v = SWAP8[(v >> 12) & 0xF] | (SWAP8[(v >> 8) & 0xF] << 4) | (SWAP8[(v >> 4) & 0xF] << 8) | (SWAP8[v & 0xF] << 12)
        return (code << 15) | v

    def _renormalise(self, low, high):
        bit_count = READ_COUNT[((low >> 12) ^ (high >> 12)) & 0xF]
        mask = FAST_NOT_MASK[bit_count]
        low = ((low & mask) << bit_count) & M32
        high = (((high & mask) << bit_count) | ((1 << bit_count) - 1)) & M32

        ml, mh = low & HALF, high & HALF
        while (ml | mh) == 0 or (ml == HALF and mh == HALF):
            low = (low & 0x7FFF) << 1
            high = ((high & 0x7FFF) << 1) | 1
            ml, mh = low & HALF, high & HALF
            bit_count += 1
        saved_low, saved_high = ml, mh

        if bit_count > 0:
            bit_count += self.underflow
            self.underflow = 0

        while (low & QUARTER) == QUARTER and (high & QUARTER) == 0:
            low = (low & 0x3FFF) << 1
            high = ((high & 0x3FFF) << 1) | 1
            self.underflow += 1

        self.low = low | saved_low
        self.high = high | saved_high
        self.pos += bit_count

    def _symbol_static(self, context):
        code = self._read_code()
        total = context - 1024
        low = self.low
        rng = (self.high + 1 - low) & M32
        code_range = (1 + code - low) & M32
        value = ((code_range * total - 1) // rng) & M32
        new_low = (low + ((rng * value) & M32) // total) & M32
        new_high = (low - 1 + ((rng * (value + 1)) & M32) // total) & M32
        self._renormalise(new_low, new_high)
        return value

    def _symbol_dynamic(self, context):
        code = self._read_code()
        cum = self.cum.get(context)
        total = cum[0] if cum else 1
        low = self.low
        rng = (self.high + 1 - low) & M32
        code_range = (1 + code - low) & M32
        code_freq = ((total * code_range - 1) // rng) & M32

        if cum:
            value = self._symbol_from_freq(cum, code_freq)
            value_cum = total - cum[value]
            value_freq = self.sym[context][value]
        else:
            value, value_cum, value_freq = 0, 0, 1

        new_high = (low - 1 + ((rng * (value_cum + value_freq)) & M32) // total) & M32
        new_low = (low + ((rng * value_cum) & M32) // total) & M32
        if context < STATIC_FULL:
            self._add_symbol(context, value)
        self._renormalise(new_low, new_high)
        return value

    @staticmethod
    def _symbol_from_freq(cum, freq):
        """Largest symbol whose cumulative frequency (total - cum[i]) is <= freq."""
        total = cum[0]
        if freq == 0 or total < freq:
            return 0
        need = total - freq  # cum is non-increasing: want the last i with cum[i] >= need
        lo, hi = 0, len(cum) - 1
        while lo < hi:
            mid = (lo + hi + 1) >> 1
            if cum[mid] >= need:
                lo = mid
            else:
                hi = mid - 1
        return lo

    def _add_symbol(self, context, symbol):
        if context == 0 or context > STATIC_FULL or symbol >= MAX_HIST_SYMBOL:
            return
        cum = self.cum.get(context)
        if cum is None:
            cum = self.cum[context] = [0] * (symbol + ARRAY_INCR)
            sym = self.sym[context] = [0] * (symbol + ARRAY_INCR)
            cum[0] = sym[0] = 1  # escape symbol
        else:
            sym = self.sym[context]
            if len(cum) <= symbol:
                grow = symbol + ARRAY_INCR - len(cum)
                cum.extend([0] * grow)
                sym.extend([0] * grow)

        if cum[0] >= ELEPHANT:
            acc = 0
            for i in range(len(cum) - 1, -1, -1):
                sym[i] >>= 1
                acc += sym[i]
                cum[i] = acc
            sym[0] += 1
            cum[0] += 1

        sym[symbol] += 1
        for i in range(symbol + 1):
            cum[i] += 1
