#!/usr/bin/env bash
# Static analysis of the hand-written firmware sources: clang-tidy, then cppcheck.
#   scripts/lint.sh               both
#   scripts/lint.sh clang-tidy    one of them
# Needs arm-none-eabi-gcc (for the C library headers), clang-tidy and cppcheck on the PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

CORE=STM32F103RBT6_Secure_Bootloader
DRV=$CORE/Drivers

FILES=(
  $CORE/Core/Src/bootloader.c   $CORE/Core/Src/bl_uds.c       $CORE/Core/Src/bl_isotp.c
  $CORE/Core/Src/bl_secaccess.c $CORE/Core/Src/bl_seccrypto.c $CORE/Core/Src/bl_udspolicy.c
  $CORE/Core/Src/can_bl.c       $CORE/Core/Src/flash.c        $CORE/Core/Src/flash_if.c
  $CORE/Core/Src/sbl.c          BootManager/Core/Src/main.c
)

DEFS=(-DUSE_HAL_DRIVER -DSTM32F103xB -DBL_TEST_KEYS -DUDS_SYS=UDS_SYS_CUSTOM
      -DUDS_SERVER_RECV_BUF_SIZE=256 -DUDS_SERVER_SEND_BUF_SIZE=256)

INCS=(-I$CORE/Core/Inc -I$CORE/Core/ThirdParty/isotp-c -I$CORE/Core/ThirdParty/iso14229
      -I$DRV/STM32F1xx_HAL_Driver/Inc -I$DRV/STM32F1xx_HAL_Driver/Inc/Legacy
      -I$DRV/CMSIS/Device/ST/STM32F1xx/Include -I$DRV/CMSIS/Include
      -IBootManager/Core/Inc -IBootManager/Drivers/STM32F1xx_HAL_Driver/Inc
      -IBootManager/Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
      -IBootManager/Drivers/CMSIS/Device/ST/STM32F1xx/Include -IBootManager/Drivers/CMSIS/Include
      -Itests/keys)

run_clang_tidy() {
  # clang-tidy needs the C library headers of the cross compiler: ask the compiler where they are
  local sys=()
  while IFS= read -r dir; do sys+=(-isystem "$dir"); done < <(
    arm-none-eabi-gcc -E -Wp,-v -xc /dev/null 2>&1 | sed -n '/^#include <...>/,/^End of search/p' | grep '^ ' | sed 's/^ //')
  echo "== clang-tidy"
  for f in "${FILES[@]}"; do
    # the Boot Manager has its own HAL copy and its own Core/Inc: put it first for that file
    local inc=("${INCS[@]}")
    if [[ $f == BootManager/* ]]; then inc=(-IBootManager/Core/Inc "${INCS[@]}"); fi
    clang-tidy --quiet "$f" -- --target=arm-none-eabi -mcpu=cortex-m3 -mthumb -std=c11 \
      "${sys[@]}" "${DEFS[@]}" "${inc[@]}"
  done
}

run_cppcheck() {
  echo "== cppcheck"
  cppcheck --enable=warning,performance,portability --inconclusive --std=c11 --platform=unix32 \
    --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem --quiet \
    "${DEFS[@]}" "${INCS[@]}" "${FILES[@]}"
}

case "${1:-all}" in
  clang-tidy) run_clang_tidy ;;
  cppcheck)   run_cppcheck ;;
  all)        run_clang_tidy; run_cppcheck ;;
  *) echo "usage: $0 [clang-tidy|cppcheck]"; exit 2 ;;
esac
echo "lint: clean"
