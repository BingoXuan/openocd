#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the shipped RV32I bytes against a bounded Flash/FIFO model, without USB."""
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
CODE = bytes(int(value, 16) for value in re.findall(
    r"0x([0-9a-fA-F]{2})", (ROOT / "contrib/loaders/flash/agrv2k/agrv2k.inc").read_text()))
MASK = 0xffffffff
FIFO = 0x20001000
DEST = 0x80001000
FMC = 0x40001000


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Machine:
    def __init__(self, words, mode="success"):
        self.words = words
        self.mode = mode
        self.regs = [0] * 32
        self.regs[10:16] = [FMC, len(words), FIFO, FIFO + 24, DEST, 4]
        self.regs[16] = 0x200
        self.memory = {FIFO: FIFO + 8, FIFO + 4: FIFO + 8}
        self.flash = {}
        self.pc = 0
        self.sent = 0
        self.status = 0
        self.busy_reads = 0
        self.flash_writes = []
        self.wraps = 0
        self.halted = False
        self.closed = False
        if mode == "preloaded":
            assert len(words) <= 3
            self.memory.update({FIFO + 8 + 4 * i: word for i, word in enumerate(words)})
            self.memory[FIFO] = FIFO + 8 + 4 * len(words)

    def feed(self):
        if self.mode == "preloaded":
            return
        if self.mode == "abort":
            self.memory[FIFO] = 0
            return
        head, tail = self.memory[FIFO], self.memory[FIFO + 4]
        while self.sent < len(self.words):
            following = head + 4
            if following == FIFO + 24:
                following = FIFO + 8
            if following == tail:
                break
            self.memory[head] = self.words[self.sent]
            self.sent += 1
            head = following
        self.memory[FIFO] = head

    def read(self, address):
        if address == FIFO:
            self.feed()
        if address == FMC + 12:
            if self.mode == "timeout":
                return 1
            if self.busy_reads:
                self.busy_reads -= 1
                return 1
            return self.status
        if address not in self.memory:
            raise AssertionError(f"out-of-bounds read 0x{address:08x}")
        return self.memory[address]

    def write(self, address, value):
        if address == FMC + 12:
            assert value == 0x34
            self.status = 0
        elif address == FMC + 16:
            assert value == 0x200
            self.closed = True
        elif DEST <= address < DEST + len(self.words) * 4:
            self.flash_writes.append(address)
            if self.mode == "error":
                self.status = 0x14
            else:
                self.flash[address] = value
                self.status = 0x20
            self.busy_reads = 0 if self.mode == "error" else 2
        elif address == FIFO + 4:
            if value == FIFO + 8:
                self.wraps += 1
            assert value == 0 or FIFO + 8 <= value < FIFO + 24
            self.memory[address] = value
        else:
            raise AssertionError(f"out-of-bounds write 0x{address:08x}")

    def run(self):
        for _ in range(10000):
            insn, = struct.unpack_from("<I", CODE, self.pc)
            opcode = insn & 127
            rd, funct, rs1, rs2 = (insn >> 7) & 31, (insn >> 12) & 7, (insn >> 15) & 31, (insn >> 20) & 31
            a, b = self.regs[rs1], self.regs[rs2]
            following = self.pc + 4
            value = None
            if opcode == 0x13:
                imm = signed(insn >> 20, 12)
                if funct == 0:
                    value = a + imm
                elif funct == 7:
                    value = a & imm
                else:
                    raise AssertionError(hex(insn))
            elif opcode == 0x37:
                value = insn & 0xfffff000
            elif opcode == 3 and funct == 2:
                value = self.read((a + signed(insn >> 20, 12)) & MASK)
            elif opcode == 0x23 and funct == 2:
                imm = ((insn >> 25) << 5) | ((insn >> 7) & 31)
                self.write((a + signed(imm, 12)) & MASK, b)
            elif opcode == 0x63:
                imm = ((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) | (((insn >> 25) & 63) << 5) | (((insn >> 8) & 15) << 1)
                assert funct in (0, 1)
                if (a == b) == (funct == 0):
                    following = self.pc + signed(imm, 13)
            elif opcode == 0x6f:
                imm = ((insn >> 31) << 20) | (((insn >> 12) & 255) << 12) | (((insn >> 20) & 1) << 11) | (((insn >> 21) & 1023) << 1)
                value = following
                following = self.pc + signed(imm, 21)
            elif opcode == 0x0f:
                pass  # Sequential model; the actual code still executes memory fences.
            elif insn == 0x00100073:
                self.halted = True
                return self.regs[10]
            else:
                raise AssertionError(f"unsupported instruction 0x{insn:08x}")
            if rd and value is not None:
                self.regs[rd] = value & MASK
            self.regs[0] = 0
            self.pc = following
        raise AssertionError("loader failed to terminate")


class LoaderTests(unittest.TestCase):
    def test_preloaded_block_without_host_memory_accesses(self):
        words = [0xff00a9c2, 0xffff6655, 0x01234567]
        machine = Machine(words, "preloaded")
        self.assertEqual(machine.run(), 0)
        self.assertEqual([machine.flash[DEST + 4 * i] for i in range(len(words))], words)
        self.assertEqual(machine.regs[14], DEST + 4 * len(words))
        self.assertTrue(machine.closed)

    def test_stream_wrap_and_busy_poll(self):
        words = [(i * 0x1234567) & MASK for i in range(37)]
        machine = Machine(words)
        self.assertEqual(machine.run(), 0)
        self.assertEqual([machine.flash[DEST + 4 * i] for i in range(len(words))], words)
        self.assertEqual(machine.regs[14], DEST + 4 * len(words))
        self.assertGreater(machine.wraps, 4)
        self.assertEqual(machine.pc, len(CODE) - 4)

    def test_controller_error_stops_after_first_word(self):
        machine = Machine([1, 2, 3], "error")
        self.assertEqual(machine.run(), 0x14)
        self.assertEqual(machine.flash_writes, [DEST])
        self.assertEqual(machine.memory[FIFO + 4], 0)

    def test_completion_deadline_closes_writer(self):
        machine = Machine([1, 2, 3], "timeout")
        self.assertEqual(machine.run(), 1)
        self.assertEqual(machine.flash_writes, [DEST, DEST + 4, DEST + 8])
        self.assertEqual(machine.memory[FIFO + 4], 0)
        self.assertTrue(machine.closed)

    def test_host_abort_does_not_program(self):
        machine = Machine([1, 2, 3], "abort")
        self.assertEqual(machine.run(), 0x80000000)
        self.assertEqual(machine.flash_writes, [])
        self.assertEqual(machine.memory[FIFO + 4], 0)


if __name__ == "__main__":
    unittest.main()
