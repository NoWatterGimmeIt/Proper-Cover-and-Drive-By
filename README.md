# Proper Cover and Drive-By

An ASI plugin for **GTA IV: The Complete Edition** that brings GTA V-style cover shooting and drive-bys to GTA IV.

The whole plugin is one source file (`src/main.cpp`). It has no dependencies and no C runtime, and it links only against `kernel32.dll` and `user32.dll`.

## Features

- **Cover:**
  - Pop-out aiming from cover (hold aim to pop out and stay out, fire only on the trigger).
  - Movement-based peeking.
  - Forced cover reload.
  - Manual crouch in tall cover, while idle, moving or aiming, with corner-only pop-outs while crouched.
  - Crouched accuracy when popped out of low cover.
  - Faster cover entry, standing cover animations +20%, and a smoother crouch-to-stand in cover.
  - Leaving cover while aiming goes straight to on-foot aiming.
- **Weapon switching while aiming:**
  - Works on foot, in cover and during drive-bys.
  - Clean sniper scope switching.
  - Switching to melee drops aim.
  - The mouse wheel is left to zoom while aiming (this can be changed in the ini).
- **Shoulder swap** while aiming on foot (LB / E, configurable).
- **GTA V-style drive-bys:**
  - Hold aim to aim and press fire to shoot.
  - Look behind (R3) while aiming.
  - Thrown weapons: aim readies, fire cooks, releasing fire throws.
- **NPC reactions:** NPCs react to being aimed at from cover and from vehicles.
- **Settings file:** `ProperCoverandDrive-By.ini` is created next to the ASI on first run.

## What the plugin does to the game (for reviewers)

All of this is in `src/main.cpp`:

- **Finds game code by byte patterns.** It scans the game's own executable code (`FindPattern`); no absolute addresses are used.
- **Patches the game process in memory.** It redirects a set of `call` instructions and virtual-table entries inside `GTAIV.exe` to its own functions, using `VirtualProtect` on the affected bytes. Nothing on disk is modified.
- **Reads input.** It uses the game's own input state, plus `GetAsyncKeyState` for the one configurable keyboard key (shoulder swap, default `E`). That key is only acted on while the player is aiming in-game, and no key data is stored or sent anywhere.
- **Uses no network access** of any kind.
- **Reads and writes only two files:**
  - `ProperCoverandDrive-By.ini` next to the ASI. It reads this with `GetPrivateProfileIntA`, and writes it only when it is missing.
  - Optionally `ProperCoverandDrive-By.log` on the Desktop, only when `Log=1`.
- **Installs a vectored exception handler** (`AddVectoredExceptionHandler`) that only writes crash details to that log. It never handles or hides exceptions.

## Building

The build script uses LLVM (clang + lld) to cross-compile a 32-bit Windows DLL. It needs `clang++`, `lld-link` and `llvm-dlltool` (LLVM 15 or newer).

**Linux / WSL**
```sh
sh build.sh
# output: out/ProperCoverandDrive-By.asi
```

**Windows**
1. Install LLVM from https://github.com/llvm/llvm-project/releases.
2. Run `build.sh` from Git Bash (with LLVM's `bin` folder on `PATH`).

The script:
1. generates small import libraries for kernel32 and user32 with `llvm-dlltool`
2. compiles `src/main.cpp` with `clang++ --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin`
3. links with `lld-link /dll /nodefaultlib /Brepro`

The build is reproducible: building v72 from this source gives a file with SHA-256
`88aa62102f40495a2f03a99d1d760ef09f5b36fa7d98b8a87d256961422752d9`, identical to the released ASI.

## Installing

1. Install an ASI loader (for example Ultimate ASI Loader).
2. Copy `ProperCoverandDrive-By.asi` into the GTA IV folder (or the `plugins` folder).
3. Start the game once. `ProperCoverandDrive-By.ini` is created next to the ASI.
