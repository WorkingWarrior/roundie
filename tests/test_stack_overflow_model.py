"""Deterministic host model of the M9 native stack overflow (M9.1).

A model, not a hardware result: it replays the write pattern of fault_elf
`recurse()` against the detectors that were tried, to show why a 32-byte
end-of-stack watchpoint can miss it and why an unmapped guard page plus a
red zone cannot.

Facts (docs/m9-stabilization.md, disassembly of fault_elf, 0.3.56): each
`recurse()` frame is 544 bytes (`entry a1, 0x220`) and the function itself
writes only `page[0]`. Assumptions of the model: the windowed ABI's 16-byte
base save area just below each frame's SP is written by window-overflow
spills, and `page[0]` lies 16 bytes above the frame's SP. The conclusions
below hold for any write offsets that leave a gap of >= 32 bytes per frame.
"""

import unittest

FRAME = 544  # entry a1, 0x220
SPILL = 16  # base save area below SP (assumption)
PAGE0 = 16  # page[0] offset above SP (assumption)
LARGEST_ENTRY = 32760  # largest immediate of one Xtensa ENTRY
INTERRUPT_FRAME = 4096  # bound used by executor_stack.c for the saved frame


def frame_writes(sp):
    """Byte ranges [low, high) one recurse() frame writes."""
    return [(sp - SPILL, sp), (sp + PAGE0, sp + PAGE0 + 1)]


def first_hit(top, region, frames):
    """Depth of the first frame writing into region=[low, high), or None."""
    low, high = region
    sp = top
    for depth in range(1, frames + 1):
        sp -= FRAME
        for write_low, write_high in frame_writes(sp):
            if write_low < high and write_high > low:
                return depth
    return None


class StackOverflowModel(unittest.TestCase):
    stack_low = 0x10000
    frames = 200  # the recursion is unbounded; deep enough to pass both regions

    def test_watchpoint_32_bytes_is_skipped_for_most_alignments(self):
        """The FreeRTOS end-of-stack watchpoint covers [stack_low, +32)."""
        window = (self.stack_low, self.stack_low + 32)
        missed = [
            offset
            for offset in range(FRAME)
            if first_hit(self.stack_low + 8 * 1024 + offset, window, self.frames) is None
        ]
        # The writes of one frame span [sp - 16, sp + 17), 33 bytes of each
        # 544-byte stride: a 32-byte window is hit for 33 + 32 - 1 = 64
        # alignments of the stack end and jumped over for the other 480.
        span = SPILL + PAGE0 + 1
        self.assertEqual(len(missed), FRAME - (span + 32 - 1))

    def test_guard_page_is_hit_for_every_alignment(self):
        guard = (self.stack_low - 64 * 1024, self.stack_low)
        for offset in range(FRAME):
            depth = first_hit(self.stack_low + 8 * 1024 + offset, guard, self.frames)
            self.assertIsNotNone(depth, offset)

    def test_red_zone_holds_any_single_entry_frame(self):
        """SP_MIN at the red-zone top: the first SP below it is still >= the
        guard top after one maximal ENTRY plus the saved interrupt frame."""
        red_zone = 40 * 1024
        self.assertGreaterEqual(red_zone, LARGEST_ENTRY + INTERRUPT_FRAME)
        limit = self.stack_low + red_zone
        for sp_before in range(limit, limit + 64, 4):
            sp_after = sp_before - LARGEST_ENTRY
            self.assertGreaterEqual(sp_after - INTERRUPT_FRAME, self.stack_low)

    def test_recurse_crosses_limit_before_guard(self):
        """Detection order for recurse(): the monitor limit is crossed many
        frames before the first write reaches the guard page."""
        red_zone = 40 * 1024
        top = self.stack_low + 64 * 1024
        limit = self.stack_low + red_zone
        crossed = next(d for d in range(1, self.frames) if top - d * FRAME < limit)
        guard_hit = first_hit(top, (self.stack_low - 64 * 1024, self.stack_low), self.frames)
        self.assertGreater(guard_hit - crossed, 70)


if __name__ == "__main__":
    unittest.main()
