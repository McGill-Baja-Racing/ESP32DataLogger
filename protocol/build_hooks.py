"""Shared PlatformIO pre-script for both firmwares.

Hashes app_protocol.h without comments or whitespace so both firmwares agree
when built from the same protocol, and copies each image to firmware/<env>.bin
at the repo root for upload from the web page.
"""
Import("env")
import hashlib
import re
import shutil
from pathlib import Path

root = Path(env.subst("$PROJECT_DIR")).parent
text = re.sub(r"/\*.*?\*/", "", (root / "protocol" / "app_protocol.h").read_text(), flags=re.DOTALL)
text = re.sub(r"//[^\n]*", "", text)
text = "".join(text.split())
protocol_hash = int(hashlib.sha256(text.encode()).hexdigest()[:4], 16)
env.Append(CPPDEFINES=[("CAN_PROTOCOL_HASH", "0x%04X" % protocol_hash)])


def copy_image(target, source, env):
    folder = root / "firmware"
    folder.mkdir(exist_ok=True)
    shutil.copyfile(str(target[0]), folder / (env.subst("$PIOENV") + ".bin"))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_image)
