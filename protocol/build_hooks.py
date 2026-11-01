"""Shared PlatformIO pre-script for both firmwares.

Hashes app_protocol.h without comments or whitespace so both firmwares agree
when built from the same protocol, and copies each vehicle image to
firmware/master.bin or firmware/node<N>.bin at the repo root for upload from
the web page. Bench-only builds are not copied.
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

flags = env.GetProjectOption("build_flags", "")
if not isinstance(flags, str):
    flags = " ".join(flags)
node_id = re.search(r"NODE_ID=(\d+)", flags)
image_name = "node%s.bin" % node_id.group(1) if node_id else "master.bin"


def copy_image(target, source, env):
    folder = root / "firmware"
    folder.mkdir(exist_ok=True)
    shutil.copyfile(str(target[0]), folder / image_name)


if not re.search(r"SENSOR_SERIAL_TEST=1\b|MASTER_CAN_ENABLED=0\b", flags):
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_image)
