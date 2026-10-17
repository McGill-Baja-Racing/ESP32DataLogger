"""Lets `pio run` build firmware on the Rust kernel: PlatformIO compiles the C, cargo adds the kernel."""

import os
import shutil
import subprocess

Import("env", "projenv")

if "PIOMAINPROG" not in env:  # `-t nobuild`
    Return()

for tool, install in (("cargo", "https://rustup.rs"), ("espflash", "cargo install espflash")):
    if not shutil.which(tool):
        print(f"{tool} not found. Install: {install}")
        env.Exit(1)

kernel_dir = Dir("..").abspath
target_dir = os.path.join(kernel_dir, "target")
app = env.StaticLibrary("$BUILD_DIR/app", env["PIOBUILDFILES"])
projenv.Append(CPPPATH=[os.path.join(kernel_dir, "include", env.subst("$BOARD_MCU"))])
unsupported = os.path.join(kernel_dir, "include", "unsupported.h")
projenv.Append(CFLAGS=["-include", unsupported])
env.Depends(env["PIOBUILDFILES"], unsupported)  # SCons scans `#include` lines, not `-include`
projenv["IDF"] = os.path.join(env.PioPlatform().get_package_dir("framework-espidf"), "components")


def link_kernel(target, source, env):
    cargo_target = env.GetProjectOption("custom_cargo_target")
    result = subprocess.run(
        ["cargo", "build", "--release", "--target", cargo_target, "--target-dir", target_dir,
         "--no-default-features", "--features", env.subst("$BOARD_MCU")],
        cwd=kernel_dir,  # so kernel/'s toolchain and cargo config apply
        env={**os.environ, "BAJA_APP_ARCHIVE": app[0].abspath},
    )
    if result.returncode == 0:
        shutil.copyfile(os.path.join(target_dir, cargo_target, "release", "baja-kernel"), str(target[0]))
    return result.returncode


env.Replace(LINKCOM=env.Action(link_kernel, "Linking kernel"))
env.Depends(env["PIOMAINPROG"], app)
env.AlwaysBuild(env["PIOMAINPROG"])  # cargo tracks the Rust sources; SCons doesn't
# The platform's map-file report reads only GNU ld maps; rust-lld's differ.
env["PIOMAINPROG"][0].get_executor().post_actions.clear()
# The size check matches ESP-IDF's section names; these are esp-hal's.
env.Replace(SIZEPROGREGEXP=r"^(?:\.trap|\.rwtext|\.data|\.flash\.appdesc|\.rodata|\.text)\s+(\d+).*",
            SIZEDATAREGEXP=r"^(?:\.data|\.bss|\.noinit)\s+(\d+).*")
if env.subst("$BOARD_MCU") == "esp32p4":  # esptool can't convert esp-hal's P4 ELF
    env.Replace(ELF2BINCMD='espflash save-image --chip $BOARD_MCU "$SOURCE" "$TARGET"')
