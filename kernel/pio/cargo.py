"""Lets `pio run` build firmware on the Rust kernel: PlatformIO compiles the C, cargo adds the kernel."""

import json
import os
import shutil
import subprocess

Import("env")

if not shutil.which("cargo"):
    print("cargo not found. Install: https://rustup.rs")
    env.Exit(1)

kernel_dir = Dir("..").abspath
# Cargo's own target folder, so `cargo clean` also cleans what pio built.
metadata = subprocess.run(["cargo", "metadata", "--format-version", "1", "--no-deps"],
                          cwd=kernel_dir, stdout=subprocess.PIPE, text=True, check=True)
target_dir = json.loads(metadata.stdout)["target_directory"]
espflash = env.GetProjectOption("custom_espflash")
# One folder per version, so changing the version in kernel.ini builds it again.
espflash_root = os.path.join(target_dir, espflash)
if not shutil.which("espflash", path=os.path.join(espflash_root, "bin")):
    print(f"Building {espflash} into {target_dir} (once per version, a few minutes)")
    # cwd so kernel/'s pinned Rust builds it.
    subprocess.run(["cargo", "install", espflash, "--locked", "--root", espflash_root],
                   cwd=kernel_dir, check=True)
env["ESPFLASH"] = os.path.join(espflash_root, "bin", "espflash")
env["ESPFLASH_FLASH_SIZE"] = env.BoardConfig().get("upload.flash_size", "4MB").lower()

if "PIOMAINPROG" not in env:  # `-t nobuild`
    Return()
Import("projenv")  # only exported when building

app = env.StaticLibrary("$BUILD_DIR/app", env["PIOBUILDFILES"])
# kernel.ini can't name paths relative to itself, so kernel/'s headers are added here.
projenv.Append(CPPPATH=[os.path.join(kernel_dir, "include", env.subst("$BOARD_MCU"))])
unsupported = projenv.File(os.path.join(kernel_dir, "include", "unsupported.h"))
projenv.Append(CFLAGS=["-include", unsupported.abspath])
# SCons scans `#include` lines, not `-include`, so the forced header and all it includes are added.
forced = unsupported.get_implicit_deps(projenv, projenv.get_scanner(".c"), lambda scanner: scanner.path(projenv))
env.Depends(env["PIOBUILDFILES"], [unsupported, *forced])


def link_kernel(target, source, env):
    cargo_target = env.GetProjectOption("custom_cargo_target")
    result = subprocess.run(
        ["cargo", "build", "--release", "--target", cargo_target, "--no-default-features",
         "--features", env.subst("$BOARD_MCU")],
        cwd=kernel_dir,  # so kernel/'s toolchain and cargo config apply
        env={**os.environ, "BAJA_APP_ARCHIVE": app[0].abspath},
    )
    if result.returncode == 0:
        shutil.copyfile(os.path.join(target_dir, cargo_target, "release", "baja-kernel"), str(target[0]))
    return result.returncode


env.Replace(LINKCOM=env.Action(link_kernel, "Linking kernel"))
env.Depends(env["PIOMAINPROG"], app)
env.AlwaysBuild(env["PIOMAINPROG"])  # cargo tracks the Rust sources; SCons doesn't
env.Replace(SIZEPROGREGEXP=env.GetProjectOption("custom_size_prog_regexp"),
            SIZEDATAREGEXP=env.GetProjectOption("custom_size_data_regexp"),
            ELF2BINCMD=env.GetProjectOption("custom_elf2bin"))
# Runs after the platform's esptool merge and overwrites its image, which has no bootloader or
# partition table; PlatformIO offers no way to remove that step.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", env.GetProjectOption("custom_factory_bin"))
