cmake_minimum_required(VERSION 3.20)
project(three_tier_bootloader C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()
# The FBL must fit in 40 KB, so size is the default optimisation goal.
set(CMAKE_C_FLAGS_MINSIZEREL "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE    "-O2 -g3")
set(CMAKE_C_FLAGS_DEBUG      "-O0 -g3")

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

  target_compile_definitions(${target}.elf PRIVATE USE_HAL_DRIVER STM32F103xB)

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
target_compile_definitions(fbl.elf PRIVATE UDS_SYS=UDS_SYS_CUSTOM)
fw(application   STM32F103RBT6_Application         startup_stm32f103rbtx.s STM32F103RBTX_FLASH.ld)
fw(bluepill_bridge BluePill_Bridge                 startup_stm32f103c8tx.s STM32F103C8TX_FLASH.ld)
