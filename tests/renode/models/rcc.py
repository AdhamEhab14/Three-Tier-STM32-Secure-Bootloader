# Minimal STM32F1 RCC: ready flags follow the enable bits, SWS follows SW.
if request.IsInit:
    regs = {0x00: 0x00000083, 0x04: 0x0}
elif request.IsWrite:
    v = request.Value
    if request.Offset == 0x00: v &= ~((1 << 1) | (1 << 17) | (1 << 25))   # RDY flags are read-only
    elif request.Offset == 0x04: v &= ~0xC                                 # SWS is read-only
    regs[request.Offset] = v
elif request.IsRead:
    v = regs.get(request.Offset, 0)
    if request.Offset == 0x00:
        if v & (1 << 0): v |= (1 << 1)      # HSION  -> HSIRDY
        if v & (1 << 16): v |= (1 << 17)    # HSEON  -> HSERDY
        if v & (1 << 24): v |= (1 << 25)    # PLLON  -> PLLRDY
    elif request.Offset == 0x04:
        v = (v & ~0xC) | ((v & 0x3) << 2)   # SWS = SW
    request.Value = v
