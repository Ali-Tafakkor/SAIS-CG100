param([Alias('Profile')][ValidateSet('internal','shadow')][string]$MemoryProfile = 'internal')
$ErrorActionPreference = 'Stop'
$sectionRoot = Split-Path $PSScriptRoot -Parent
$projectRoot = Split-Path $sectionRoot -Parent
$firmwareRoot = Join-Path $sectionRoot 'firmware'
$vendorRoot = Join-Path $projectRoot 'firmware/ethernet-api/vendor'
$cryptoRoot = Join-Path $projectRoot 'toolchain/crypto/mbedtls-3.6.5'
$buildRoot = Join-Path $firmwareRoot 'build'
if ($MemoryProfile -eq 'shadow') { $buildRoot = Join-Path $firmwareRoot 'build-shadow' }
$memoryRoot = Join-Path $projectRoot 'firmware/memory-platform'
$objectRoot = Join-Path $buildRoot 'obj'
$compilerBin = Join-Path $projectRoot 'toolchain/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin'
if ($env:G100_COMPILER_BIN) { $compilerBin = $env:G100_COMPILER_BIN }
$gcc = Join-Path $compilerBin 'arm-none-eabi-gcc.exe'
$objcopy = Join-Path $compilerBin 'arm-none-eabi-objcopy.exe'
$size = Join-Path $compilerBin 'arm-none-eabi-size.exe'
foreach ($binary in @($gcc,$objcopy,$size)) {
    if (-not (Test-Path -LiteralPath $binary)) { throw "Toolchain binary missing: $binary" }
}
New-Item -ItemType Directory -Force -Path $objectRoot | Out-Null
$manifest = Join-Path $buildRoot 'build-status.json'
@{ success = $false; started = (Get-Date).ToString('o') } |
    ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8

$includes = @(
    $memoryRoot,
    (Join-Path $cryptoRoot 'include'),
    (Join-Path $cryptoRoot 'library'),
    (Join-Path $firmwareRoot 'app'),
    (Join-Path $firmwareRoot 'vendor/LWIP/App'),
    (Join-Path $firmwareRoot 'vendor/LWIP/Target'),
    (Join-Path $vendorRoot 'Core/Inc'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Inc'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Inc/Legacy'),
    (Join-Path $vendorRoot 'Drivers/CMSIS/Device/ST/STM32H7xx/Include'),
    (Join-Path $vendorRoot 'Drivers/CMSIS/Include'),
    (Join-Path $vendorRoot 'Drivers/BSP/Components/lan8742'),
    (Join-Path $vendorRoot 'LWIP/Target'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/include'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/system'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/include/netif/ppp')
) | ForEach-Object { '-I' + $_ }

$common = @(
    '-mcpu=cortex-m7','-mthumb','-mfpu=fpv5-d16','-mfloat-abi=hard',
    '-DSTM32H750xx','-DUSE_HAL_DRIVER','-Oz','-g3','-flto',
    '-DMBEDTLS_CONFIG_FILE=<rj_crypto_config.h>',
    '-ffunction-sections','-fdata-sections','-fno-common',
    '-Wall','-Wextra','-Wno-unused-parameter'
) + $includes
if ($MemoryProfile -eq 'shadow') { $common += '-DG100_SHADOW=1' }

$sources = @(
    (Join-Path $firmwareRoot 'app/main.c'),
    (Join-Path $firmwareRoot 'app/http_api.c'),
    (Join-Path $firmwareRoot 'app/led_control.c'),
    (Join-Path $firmwareRoot 'app/device_identity.c'),
    (Join-Path $firmwareRoot 'app/discovery.c'),
    (Join-Path $firmwareRoot 'app/access_control.c'),
    (Join-Path $firmwareRoot 'app/network_config.c'),
    (Join-Path $firmwareRoot 'app/rj_api.c'),
    (Join-Path $firmwareRoot 'app/rj_json.c'),
    (Join-Path $firmwareRoot 'app/rj_catalogue.c'),
    (Join-Path $firmwareRoot 'app/rj_security.c'),
    (Join-Path $firmwareRoot 'startup.s'),
    (Join-Path $vendorRoot 'Core/Src/system_stm32h7xx.c'),
    (Join-Path $vendorRoot 'Core/Src/stm32h7xx_hal_msp.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_eth.c'),
    (Join-Path $vendorRoot 'Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_eth_ex.c'),
    (Join-Path $vendorRoot 'Drivers/BSP/Components/lan8742/lan8742.c'),
    (Join-Path $firmwareRoot 'vendor/LWIP/App/lwip.c'),
    (Join-Path $vendorRoot 'LWIP/Target/ethernetif.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/def.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/dns.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/init.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/inet_chksum.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ip.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/mem.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/memp.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/netif.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/pbuf.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/raw.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/stats.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/sys.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/tcp.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/tcp_in.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/tcp_out.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/udp.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/timeouts.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ipv4/icmp.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ipv4/ip4.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_addr.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_frag.c'),
    (Join-Path $vendorRoot 'Middlewares/Third_Party/LwIP/src/netif/ethernet.c')
)
$cryptoSources = 'platform','platform_util','sha256','md','pkcs5','bignum','bignum_core','bignum_mod','bignum_mod_raw','ecp','ecp_curves','ecdsa','asn1parse','asn1write','oid','pk','pk_ecc','pk_wrap','pkparse','pkwrite','pem','base64','x509','x509_crt','x509_create','x509write_csr','constant_time'
foreach($name in $cryptoSources) { $sources += Join-Path $cryptoRoot "library/$name.c" }
if ($MemoryProfile -eq 'shadow') {
    $sources = @($sources | Where-Object { $_ -ne (Join-Path $firmwareRoot 'startup.s') })
    $sources += Join-Path $firmwareRoot 'startup-shadow.s'
    $sources += Join-Path $firmwareRoot 'app/architecture.c'
    $sources += Join-Path $memoryRoot 'board_memory.c'
    $sources += Join-Path $memoryRoot 'boot_state.c'
}

$objects = @()
foreach ($source in $sources) {
    $objectName = (($source.Substring($projectRoot.Length)) -replace '[\\/:.]','_') + '.o'
    $object = Join-Path $objectRoot $objectName
    & $gcc @common '-c' $source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
    $objects += $object
}

$elf = Join-Path $buildRoot 'g100-api.elf'
$map = Join-Path $buildRoot 'g100-api.map'
$linker = Join-Path $firmwareRoot 'stm32h750.ld'
if ($MemoryProfile -eq 'shadow') { $linker = Join-Path $firmwareRoot 'stm32h750-shadow.ld' }
& $gcc '-mcpu=cortex-m7' '-mthumb' '-mfpu=fpv5-d16' '-mfloat-abi=hard' `
    '-flto' '-Oz' `
    '-specs=nano.specs' '-specs=nosys.specs' @objects `
    "-T$linker" '-Wl,--gc-sections' "-Wl,-Map=$map" '-Wl,--print-memory-usage' `
    '-Wl,--start-group' '-lc' '-lm' '-Wl,--end-group' '-o' $elf
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
& $objcopy '-O' 'binary' $elf (Join-Path $buildRoot 'g100-api.bin')
if ($LASTEXITCODE -ne 0) { throw 'Binary conversion failed.' }
& $objcopy '-O' 'ihex' $elf (Join-Path $buildRoot 'g100-api.hex')
if ($LASTEXITCODE -ne 0) { throw 'Hex conversion failed.' }
& $size $elf
if ($LASTEXITCODE -ne 0) { throw 'Size check failed.' }
@{ success = $true; profile = $MemoryProfile; finished = (Get-Date).ToString('o');
   sha256 = (Get-FileHash -LiteralPath (Join-Path $buildRoot 'g100-api.bin')).Hash } |
    ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
