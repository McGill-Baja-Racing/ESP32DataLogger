"""Lets `pio run` build firmware on the Rust kernel: PlatformIO compiles the C, cargo adds the kernel."""

import os
import shutil
import subprocess

Import("env", "projenv")  # PlatformIO build settings: for everything, and for the project C files

if "PIOMAINPROG" not in env:  # `-t nobuild`: nothing to link
    Return()

kernel_dir = Dir("..").abspath  # kernel/, the folder above this script
target_dir = os.path.join(kernel_dir, "target")  # where cargo puts what it builds
app = env.StaticLibrary("$BUILD_DIR/app", env["PIOBUILDFILES"])  # bundle the compiled C into one file, libapp.a
projenv.Append(CPPPATH=[os.path.join(kernel_dir, "include", env.subst("$BOARD_MCU"))])  # this chip's sdkconfig.h
projenv["IDF"] = os.path.join(env.PioPlatform().get_package_dir("framework-espidf"), "components")  # kernel.ini's $IDF


def link_kernel(target, source, env):  # PlatformIO calls this instead of its usual last step
    cargo_target = env.GetProjectOption("custom_cargo_target")  # which chip to build for, from kernel.ini
    result = subprocess.run(
        ["cargo", "build", "--release", "--target", cargo_target, "--target-dir", target_dir,
         "--no-default-features", "--features", env.subst("$BOARD_MCU")],  # build the kernel for this chip
        cwd=kernel_dir,  # run inside kernel/ so its Rust settings apply
        env={**os.environ, "BAJA_APP_ARCHIVE": app[0].abspath},  # tell build.rs where libapp.a is
    )
    if result.returncode == 0:  # it worked: put the result where PlatformIO expects it
        shutil.copyfile(os.path.join(target_dir, cargo_target, "release", "baja-kernel"), str(target[0]))
    return result.returncode  # an error here stops the PlatformIO build


env.Replace(LINKCOM=env.Action(link_kernel, "Linking kernel"))  # use link_kernel as the last step
env.Depends(env["PIOMAINPROG"], app)  # make libapp.a first
env.AlwaysBuild(env["PIOMAINPROG"])  # run cargo every time; it skips work if nothing changed
env.Replace(ELF2BINCMD='espflash save-image --chip $BOARD_MCU "$SOURCE" "$TARGET"')  # esptool can't convert esp-hal's P4 ELF
