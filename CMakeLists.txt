cmake_minimum_required(VERSION 3.20)
project(three_tier_bootloader C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()
set(BL_SEC_KEY_HEADER "" CACHE FILEPATH "Header defining BL_SEC_KEY_BYTES: the SecurityAccess key to build in (default: the public demo key)")
option(BL_TEST_KEYS "Build the FBL with the published TEST keys (emulator/CI only, never a release)" OFF)
option(BL_ALLOW_DEMO_KEY "Let the FBL build with the PUBLIC demo SecurityAccess key when no BL_SEC_KEY_HEADER is given" OFF)

# The FBL must fit in 40 KB, so size is the default optimisation goal.
set(CMAKE_C_FLAGS_MINSIZEREL "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE    "-O2 -g3")
# -Og, not -O0: at -O0 the FBL no longer fits its 40 KB region
set(CMAKE_C_FLAGS_DEBUG      "-Og -g3")

# One firmware target per project folder. Each project owns its HAL copy,
# linker script and startup file, exactly as STM32CubeIDE lays them out.
#   fw(<target> <folder> <startup> <linker script> [extra include dirs...])
function(fw target dir startup ld)
  file(GLOB core_src   CONFIGURE_DEPENDS ${dir}/Core/Src/*.c)
  file(GLOB hal_src    CONFIGURE_DEPENDS ${dir}/Drivers/STM32F1xx_HAL_Driver/Src/*.c)
  set(extra_src "")
  foreach(inc IN LISTS ARGN)
    file(GLOB more CONFIGURE_DEPENDS ${dir}/${inc}/*.c)
    list(APPEND extra_src ${more})
  endforeach()

  add_executable(${target}.elf ${core_src} ${hal_src} ${extra_src} ${dir}/Core/Startup/${startup})

  target_include_directories(${target}.elf PRIVATE
    ${dir}/Core/Inc
    ${dir}/Drivers/STM32F1xx_HAL_Driver/Inc
    ${dir}/Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
    ${dir}/Drivers/CMSIS/Device/ST/STM32F1xx/Include
    ${dir}/Drivers/CMSIS/Include)
  foreach(inc IN LISTS ARGN)
    target_include_directories(${target}.elf PRIVATE ${dir}/${inc})
  endforeach()

  # same symbols the CubeIDE project defines (it adds DEBUG only to its Debug configuration)
  target_compile_definitions(${target}.elf PRIVATE USE_HAL_DRIVER STM32F103xB $<$<CONFIG:Debug>:DEBUG>)

  set(cpu -mcpu=cortex-m3 -mthumb)
  target_compile_options(${target}.elf PRIVATE ${cpu}
    -ffunction-sections -fdata-sections -fstack-usage -Wall)
  target_link_options(${target}.elf PRIVATE ${cpu}
    -T${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${ld}
    -Wl,-Map=${target}.map,--gc-sections
    --specs=nano.specs --specs=nosys.specs
    -Wl,--start-group -lc -lm -Wl,--end-group)
  set_target_properties(${target}.elf PROPERTIES LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${ld})
  set_source_files_properties(${dir}/Core/Startup/${startup}
    PROPERTIES COMPILE_OPTIONS "${cpu};-x;assembler-with-cpp")

  add_custom_command(TARGET ${target}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}.elf> ${target}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}.elf>
    COMMENT "${target}: .bin + size")
endfunction()

fw(boot_manager  BootManager                       startup_stm32f103rbtx.s STM32F103RBTX_FLASH.ld)
fw(fbl           STM32F103RBT6_Secure_Bootloader   startup_stm32f103rbtx.s STM32F103RBTX_FLASH.ld
                 Core/ThirdParty/isotp-c Core/ThirdParty/iso14229)
# iso14229 asks the integrator to name the platform; this is its documented default.
target_compile_definitions(fbl.elf PRIVATE UDS_SYS=UDS_SYS_CUSTOM
                           UDS_SERVER_RECV_BUF_SIZE=256 UDS_SERVER_SEND_BUF_SIZE=256)   # as in the Cube project
option(BL_UDS_SELFTEST "Boot into the iso14229 server's on-chip self-test instead of the bootloader (LD2 solid = pass)" OFF)
if(BL_UDS_SELFTEST)
  target_compile_definitions(fbl.elf PRIVATE BL_UDS_SELFTEST_ON_BOOT=1)
endif()
if(BL_TEST_KEYS)
  target_compile_definitions(fbl.elf PRIVATE BL_TEST_KEYS)
  target_include_directories(fbl.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/keys)
elseif(BL_SEC_KEY_HEADER)
  # a deployment's own SecurityAccess key (header defining BL_SEC_KEY_BYTES)
  get_filename_component(_sec_key_header "${BL_SEC_KEY_HEADER}" ABSOLUTE)
  target_compile_definitions(fbl.elf PRIVATE "BL_SEC_KEY_HEADER=\"${_sec_key_header}\"")
else()
  # the demo key is public, so a build must ask for it by name instead of getting it by accident
  if(NOT BL_ALLOW_DEMO_KEY)
    message(FATAL_ERROR "No SecurityAccess key given. Pass -DBL_SEC_KEY_HEADER=path/to/key.h (python host/sign_tool.py genseckey writes one), or -DBL_ALLOW_DEMO_KEY=ON for a demo build with the PUBLIC key.")
  endif()
  message(WARNING "Building with the PUBLIC demo SecurityAccess key.")
endif()
fw(application   STM32F103RBT6_Application         startup_stm32f103rbtx.s STM32F103RBTX_FLASH.ld)
fw(bluepill_bridge BluePill_Bridge                 startup_stm32f103c8tx.s STM32F103C8TX_FLASH.ld)
# Throwaway two-board test: the Blue Pill becomes a UDS client for the Nucleo's iso14229 server.
option(BP_UDS_CLIENT "Build the Blue Pill as a UDS test client (uses the FBL's seccrypto for SecurityAccess)" OFF)
if(BP_UDS_CLIENT)
  set(_fbl_core ${CMAKE_CURRENT_SOURCE_DIR}/STM32F103RBT6_Secure_Bootloader/Core)
  target_sources(bluepill_bridge.elf PRIVATE ${_fbl_core}/Src/bl_seccrypto.c)
  target_include_directories(bluepill_bridge.elf PRIVATE ${_fbl_core}/Inc)
  target_compile_definitions(bluepill_bridge.elf PRIVATE BP_UDS_CLIENT_ON_BOOT=1)
  if(BL_TEST_KEYS)
    target_compile_definitions(bluepill_bridge.elf PRIVATE BL_TEST_KEYS)
    target_include_directories(bluepill_bridge.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/keys)
  elseif(BL_SEC_KEY_HEADER)
    target_compile_definitions(bluepill_bridge.elf PRIVATE "BL_SEC_KEY_HEADER=\"${_sec_key_header}\"")
  endif()
endif()

# Strict checking of the hand-written sources (not the generated Cube code, the HAL or the vendored
# libraries): extra warnings as errors, and GCC's static analyzer.
option(BL_STRICT "Strict warnings as errors and -fanalyzer on our own sources" OFF)
if(BL_STRICT)
  set(_core ${CMAKE_CURRENT_SOURCE_DIR}/STM32F103RBT6_Secure_Bootloader/Core/Src)
  set(_own
    ${_core}/bootloader.c ${_core}/bl_uds.c ${_core}/bl_isotp.c ${_core}/bl_secaccess.c
    ${_core}/bl_seccrypto.c ${_core}/bl_udspolicy.c ${_core}/can_bl.c ${_core}/flash.c
    ${_core}/flash_if.c ${_core}/sbl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BootManager/Core/Src/main.c)
  set_source_files_properties(${_own} PROPERTIES COMPILE_OPTIONS
    "-Wextra;-Wshadow;-Wpointer-arith;-Wcast-align;-Wstrict-prototypes;-Wformat=2;-fanalyzer;-Werror")
endif()
