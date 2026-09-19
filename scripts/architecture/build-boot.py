"""Build the internal Stage 0 and Flash-free RAM initializer with project-local GCC."""
from pathlib import Path
import subprocess
import json
import hashlib
import os
ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'firmware/ethernet-api/vendor'
PLATFORM = ROOT / 'firmware/memory-platform'
BOOT = ROOT / 'firmware/bootloader'
BIN = ROOT / 'toolchain/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin'
if os.environ.get('G100_COMPILER_BIN'):
    BIN = Path(os.environ['G100_COMPILER_BIN'])
OUT = BOOT / 'build'
OUT.mkdir(exist_ok=True)
includes = [PLATFORM, ROOT/'SetAPIs_Calling/firmware/app', VENDOR/'Core/Inc',
            VENDOR/'Drivers/STM32H7xx_HAL_Driver/Inc',VENDOR/'Drivers/STM32H7xx_HAL_Driver/Inc/Legacy',
            VENDOR/'Drivers/CMSIS/Include',VENDOR/'Drivers/CMSIS/Device/ST/STM32H7xx/Include']
common = ['-mcpu=cortex-m7','-mthumb','-mfpu=fpv5-d16','-mfloat-abi=hard','-DSTM32H750xx','-DUSE_HAL_DRIVER',
          '-Os','-g3','-flto','-ffunction-sections','-fdata-sections','-fno-common','-Wall','-Wextra','-Wno-unused-parameter']
common += ['-I'+str(p) for p in includes]
hal = ['stm32h7xx_hal','stm32h7xx_hal_cortex','stm32h7xx_hal_gpio','stm32h7xx_hal_rcc',
       'stm32h7xx_hal_rcc_ex','stm32h7xx_hal_pwr','stm32h7xx_hal_pwr_ex','stm32h7xx_hal_flash','stm32h7xx_hal_flash_ex']
sources = [BOOT/'startup.s', BOOT/'main.c',PLATFORM/'board_clock.c',PLATFORM/'board_memory.c',PLATFORM/'boot_state.c',
           VENDOR/'Core/Src/system_stm32h7xx.c']
sources += [VENDOR/'Drivers/STM32H7xx_HAL_Driver/Src'/(s+'.c') for s in hal]
for name, linker, defines in [('stage0','stm32h750-boot.ld',[]),('ram-init','ram-init.ld',['-DG100_RAM_INIT=1'])]:
    objects=[]
    for source in sources:
        obj=OUT/(name+'-'+source.stem+'.o')
        subprocess.run([str(BIN/'arm-none-eabi-gcc.exe'),*common,*defines,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
    elf=OUT/(name+'.elf')
    subprocess.run([str(BIN/'arm-none-eabi-gcc.exe'),*common,*defines,*objects,'-T'+str(BOOT/linker),
                    '-specs=nano.specs','-specs=nosys.specs','-Wl,--gc-sections','-Wl,--build-id=none',
                    '-Wl,-Map='+str(OUT/(name+'.map')),'-Wl,--print-memory-usage','-o',str(elf)],check=True)
    image=OUT/(name+'.bin')
    subprocess.run([str(BIN/'arm-none-eabi-objcopy.exe'),'-O','binary',str(elf),str(image)],check=True)
    subprocess.run([str(BIN/'arm-none-eabi-size.exe'),str(elf)],check=True)
    (OUT/(name+'-build.json')).write_text(json.dumps({'bytes':image.stat().st_size,'sha256':hashlib.sha256(image.read_bytes()).hexdigest()}))
