# Boot-only bxCAN stand-in: just the MCR/MSR init and sleep handshake.
# Works around Renode STMCAN ignoring INRQ while SLEEP is set (renode/renode issue, 2026-10).
if request.IsInit:
    regs = {0x000: 0x00010002}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    if request.Offset == 0x004:
        mcr = regs.get(0x000, 0)
        msr = 0
        if mcr & 1: msr |= 1      # INRQ  -> INAK
        if mcr & 2: msr |= 2      # SLEEP -> SLAK
        request.Value = msr
    else:
        request.Value = regs.get(request.Offset, 0)
