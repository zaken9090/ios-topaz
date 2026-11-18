# Build the lavender shim -> shim\out\shim-boot.img
# Needs LLVM (clang, ld.lld, llvm-objcopy) and Python.
#   build.ps1                      probe shim (no kernel)
#   build.ps1 -Payload payload.bin shim + XNU payload from tools\mkpayload.py
param([string]$Payload)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$L = 'C:\Program Files\LLVM\bin'
$cflags = @('--target=aarch64-none-elf', '-O2', '-ffreestanding', '-nostdlib', '-fno-pic',
            '-mgeneral-regs-only', '-mstrict-align', '-fno-stack-protector', '-Wall', '-Wextra')
New-Item -ItemType Directory -Force out | Out-Null
& "$L\clang.exe" @cflags -c start.S -o out\start.o; if ($LASTEXITCODE) { throw 'start.S' }
& "$L\clang.exe" @cflags -c shim.c -o out\shim.o; if ($LASTEXITCODE) { throw 'shim.c' }
& "$L\clang.exe" @cflags -c mmu.c -o out\mmu.o; if ($LASTEXITCODE) { throw 'mmu.c' }
# Runs wherever ABL loads it, no relocation pass: C code must not hold absolute addresses.
$abs = (& "$L\llvm-readobj.exe" -r out\shim.o out\mmu.o | Select-String 'R_AARCH64_ABS64').Count
if ($abs) { throw "$abs absolute relocations in C objects" }
& "$L\ld.lld.exe" -T shim.ld --build-id=none -N -o out\shim.elf out\start.o out\shim.o out\mmu.o; if ($LASTEXITCODE) { throw 'link' }
& "$L\llvm-objcopy.exe" -O binary out\shim.elf out\Image; if ($LASTEXITCODE) { throw 'objcopy' }
# Pad to _end so .bss and stacks exist as zeroed bytes inside image_size.
$end = [Convert]::ToInt64(((& "$L\llvm-nm.exe" out\shim.elf | Select-String ' _end$').Line -split ' ')[0], 16)
$fs = [IO.File]::Open("$PSScriptRoot\out\Image", 'Open'); $fs.SetLength($end); $fs.Close()
if ($Payload) {
	# XNU payload (tools/mkpayload.py) at the first 4K boundary after _end; image_size grows (loader find_payload)
	python -c "import os,struct,sys; f=open(sys.argv[1],'r+b'); f.seek(0,2); n=f.tell(); f.write(b'\0'*((-n)&0xfff)); f.write(open(sys.argv[2],'rb').read()); sz=f.tell(); f.seek(16); f.write(struct.pack('<Q',sz)); print('payload appended, image', hex(sz))" out\Image $Payload
}
# Real lavender DTB (cut from the official TWRP image by tools\dtbinfo.py); not in git.
# Default: the project folder that holds this repo, ..\..\..\test\twrp-dtb\dtb0.dtb
$dtb = if ($env:LAVENDER_DTB) { $env:LAVENDER_DTB } else { "$PSScriptRoot\..\..\..\test\twrp-dtb\dtb0.dtb" }
python ..\tools\mkboot.py out\Image $dtb out\shim-boot.img

