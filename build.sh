#!/bin/sh
# Builds GTAIVCoverAim.asi (32-bit Windows DLL) with clang + lld on Linux.
set -e
cd "$(dirname "$0")"
mkdir -p out
cat > out/kernel32.def <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetModuleHandleA@4
VirtualProtect@16
VirtualAlloc@16
DisableThreadLibraryCalls@4
GetModuleFileNameA@12
GetEnvironmentVariableA@12
GetPrivateProfileIntA@16
WritePrivateProfileStringA@16
GetFileAttributesA@4
CreateFileA@28
WriteFile@20
CloseHandle@4
AddVectoredExceptionHandler@8
GetModuleHandleExA@12
LoadLibraryA@4
GetProcAddress@8
GetCurrentProcessId@0
EOF
cat > out/user32.def <<'EOF'
LIBRARY user32.dll
EXPORTS
GetAsyncKeyState@4
GetForegroundWindow@0
GetWindowThreadProcessId@8
EOF
llvm-dlltool -m i386 -d out/user32.def -l out/user32.lib -k
llvm-dlltool -m i386 -d out/kernel32.def -l out/kernel32.lib -k
clang++ --target=i686-pc-windows-msvc -O2 -msse2 -std=c++17 \
  -fno-exceptions -fno-rtti -ffreestanding -fno-builtin -fno-stack-protector \
  -Wall -c src/main.cpp -o out/main.obj
lld-link /dll /nodefaultlib /Brepro /entry:DllMain /safeseh:no /out:out/ProperCoverandDrive-By.asi \
  out/main.obj out/kernel32.lib out/user32.lib
echo "Built out/ProperCoverandDrive-By.asi"
