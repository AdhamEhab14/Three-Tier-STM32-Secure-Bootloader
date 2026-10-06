# Minimal STM32F1 FPEC: lock/unlock, page and mass erase, always ready.
PAGE = 0x400
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x20000
if request.IsInit:
    regs = {0x10: 0x80, 0x0C: 0x0, 0x14: 0, 0x04: 0}
    keystate = 0
elif request.IsWrite:
    o, v = request.Offset, request.Value
    if o == 0x04:
        if keystate == 0 and v == 0x45670123: keystate = 1
        elif keystate == 1 and v == 0xCDEF89AB:
            keystate = 2; regs[0x10] &= ~0x80
        else: keystate = 0
    elif o == 0x10:
        locked = regs[0x10] & 0x80
        if v & 0x80:                    # LOCK is write-1-to-set and restarts the key sequence
            locked = 0x80
            keystate = 0
        regs[0x10] = (v & ~(0x80 | 0x40)) | locked   # STRT clears itself when the operation ends
        if (not locked) and (v & 0x40):  # STRT
            if v & 0x2:                  # PER
                a = regs[0x14] & ~(PAGE - 1)
                for i in range(0, PAGE, 4):
                    self.GetMachine().SystemBus.WriteDoubleWord(a + i, 0xFFFFFFFF)
            elif v & 0x4:                # MER
                for i in range(0, FLASH_SIZE, 4):
                    self.GetMachine().SystemBus.WriteDoubleWord(FLASH_BASE + i, 0xFFFFFFFF)
    elif o == 0x0C:
        pass                             # flags are write-1-to-clear; we never set errors
    else:
        regs[o] = v
elif request.IsRead:
    o = request.Offset
    if o == 0x0C: request.Value = 0x20   # EOP set, BSY clear
    elif o == 0x1C: request.Value = 0x03FFFFFC  # OBR: no read protection
    else: request.Value = regs.get(o, 0)
