#!/usr/bin/env bash
# Flash size of a firmware ELF against the room it has.
#   scripts/size_check.sh <elf> <limit-bytes> [warn-below-bytes]
# Prints the headroom, warns (GitHub annotation) when it drops under the margin and fails when
# the image no longer fits. The linker already refuses an overflow; the warning is what gives
# notice before the next small change breaks a build.
set -euo pipefail
elf=$1; limit=$2; margin=${3:-512}

used=$(arm-none-eabi-size "$elf" | awk 'NR==2 { print $1 + $2 }')   # text + data is what lands in flash
left=$((limit - used))
echo "$(basename "$elf"): $used of $limit bytes, $left free"

if (( left < 0 )); then
  echo "::error::$(basename "$elf") does not fit ($((-left)) bytes over)"
  exit 1
elif (( left < margin )); then
  echo "::warning::$(basename "$elf") has only $left bytes free (margin $margin)"
fi
