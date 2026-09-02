# Vendored third-party libraries

These libraries are vendored (source copied in), not submoduled, so the
STM32CubeIDE project stays self-contained and builds without extra clone steps.
Only the files needed for the firmware build are included; tests, examples and
build scripts from upstream are omitted.

## isotp-c  (ISO 15765-2 transport layer)

- Upstream: https://github.com/SimonCahill/isotp-c
- Commit:   abb9e552df0e7ca0148c146124795341d57124fe
- License:  MIT (see isotp-c/LICENSE)
- Files:    isotp.c, isotp.h, isotp_config.h, isotp_defines.h, isotp_user.h

User callbacks the firmware must provide (declared in isotp_user.h):
- isotp_user_send_can()  -> transmit one CAN frame
- isotp_user_get_us()    -> monotonic microsecond timestamp
- isotp_user_debug()     -> debug/trace sink

## iso14229  (ISO 14229-1 UDS, client + server)

- Upstream: https://github.com/driftregion/iso14229
- Commit:   f5f7c5c362e11ee2905aef8d82670c8b3b03e513
- License:  MIT (see iso14229/LICENSE)
- Files:    iso14229.c, iso14229.h  (upstream single-file amalgamation)

## Notes

- Vendored as-is; no upstream source was modified. Any integration lives in the
  firmware's own glue files under Core/Src, not inside these folders.
- To update: re-copy the listed files from the pinned commit above (or a newer
  one) and update the commit hashes here.
