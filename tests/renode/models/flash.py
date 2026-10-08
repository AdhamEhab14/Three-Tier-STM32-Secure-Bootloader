# STM32F1 flash controller (FPEC) for the emulator: lock/unlock, page and mass
# erase, always ready. It also numbers every flash operation and can stop the
# machine at a chosen one, which is how the power-cut tests "pull the plug".
#
# An operation starts when software sets STRT (erase) or sets PG (program). The
# halfword itself is written by the CPU straight into flash memory afterwards.
#
# Test-only registers (not on the real chip):
#   0x100  opcount     operations started so far (read) / set (write)
#   0x104  cutpoint    pause when opcount reaches this value (0xFFFFFFFF = never)
#   0x108  cut info    read: 1 when the cut fired
#   0x10C  cut kind    read: 1 = erase, 2 = program, 0 = none
#   0x110  cut addr    read: page address of the erase that was cut (0 for a program)
#   0x114  cut page    pause when an erase of this page is about to start (0xFFFFFFFF = never)
PAGE = 0x400
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x20000
NEVER = 0xFFFFFFFF

if request.IsInit:
    regs = {0x10: 0x80, 0x0C: 0x0, 0x14: 0, 0x04: 0}
    keystate = 0
    opcount = 0
    cutpoint = NEVER
    cut_hit = 0
    cut_kind = 0
    cut_addr = 0
    cut_page = NEVER
elif request.IsWrite:
    o, v = request.Offset, request.Value
    if o == 0x04:
        if keystate == 0 and v == 0x45670123: keystate = 1
        elif keystate == 1 and v == 0xCDEF89AB:
            keystate = 2; regs[0x10] &= ~0x80
        else: keystate = 0
    elif o == 0x10:
        old = regs[0x10]
        locked = old & 0x80
        if v & 0x80:                    # LOCK is write-1-to-set and restarts the key sequence
            locked = 0x80
            keystate = 0
        regs[0x10] = (v & ~(0x80 | 0x40)) | locked   # STRT clears itself when the operation ends
        unlocked = not locked
        starting_erase = unlocked and (v & 0x40) and (v & 0x6)
        starting_prog = unlocked and (v & 0x1) and not (old & 0x1)
        if starting_erase or starting_prog:
            page_now = (regs[0x14] & ~(PAGE - 1)) if starting_erase else NEVER
            if (opcount == cutpoint or (starting_erase and page_now == cut_page)) and not cut_hit:
                cut_hit = 1
                cut_kind = 1 if starting_erase else 2
                cut_addr = (regs[0x14] & ~(PAGE - 1)) if starting_erase else 0
                for c in self.GetMachine().SystemBus.GetCPUs():
                    c.IsHalted = True            # power is gone: stop executing right here
            elif not cut_hit:
                opcount += 1
                if starting_erase:
                    if v & 0x2:          # PER
                        a = regs[0x14] & ~(PAGE - 1)
                        for i in range(0, PAGE, 4):
                            self.GetMachine().SystemBus.WriteDoubleWord(a + i, 0xFFFFFFFF)
                    else:                # MER
                        for i in range(0, FLASH_SIZE, 4):
                            self.GetMachine().SystemBus.WriteDoubleWord(FLASH_BASE + i, 0xFFFFFFFF)
    elif o == 0x0C:
        pass                             # flags are write-1-to-clear; we never set errors
    elif o == 0x100:
        opcount = v
    elif o == 0x114:
        cut_page = v
        cut_hit = 0
        cut_kind = 0
        cut_addr = 0
    elif o == 0x104:
        cutpoint = v
        cut_hit = 0
        cut_kind = 0
        cut_addr = 0
    else:
        regs[o] = v
elif request.IsRead:
    o = request.Offset
    if o == 0x0C: request.Value = 0x20   # EOP set, BSY clear
    elif o == 0x1C: request.Value = 0x03FFFFFC  # OBR: no read protection
    elif o == 0x100: request.Value = opcount
    elif o == 0x104: request.Value = cutpoint
    elif o == 0x108: request.Value = cut_hit
    elif o == 0x10C: request.Value = cut_kind
    elif o == 0x110: request.Value = cut_addr
    elif o == 0x114: request.Value = cut_page
    else: request.Value = regs.get(o, 0)
