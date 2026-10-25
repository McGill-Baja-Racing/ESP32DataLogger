"""Hashes app_protocol.h without comments or whitespace so both firmwares agree when built from the same protocol."""
Import("env")
import hashlib
import re
from pathlib import Path

header = Path(env.subst("$PROJECT_DIR")) / ".." / "protocol" / "app_protocol.h"
text = re.sub(r"/\*.*?\*/", "", header.read_text(), flags=re.DOTALL)
text = re.sub(r"//[^\n]*", "", text)
text = "".join(text.split())
protocol_hash = int(hashlib.sha256(text.encode()).hexdigest()[:4], 16)
env.Append(CPPDEFINES=[("CAN_PROTOCOL_HASH", "0x%04X" % protocol_hash)])
