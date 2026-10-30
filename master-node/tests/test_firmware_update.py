"""Host checks: python3 -m unittest discover -s tests -p test_firmware_update.py"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/update/firmware_update.c").read_text()
DEFINES = SOURCE[SOURCE.index("#define IMAGE_MAGIC"):SOURCE.index("static bool parse_image(")]
START = SOURCE.index("static bool parse_image(")
FUNCTION = SOURCE[START:SOURCE.index("\n}\n", START) + 3]
HARNESS = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "protocol/app_protocol.h"
''' + DEFINES + FUNCTION + r'''
int main(int argc, char **argv)
{
    (void)argc;
    static uint8_t image[512];
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    size_t length = fread(image, 1, sizeof(image), file);
    fclose(file);
    firmware_identity_t identity;
    uint8_t hash[NODE_IDENTITY_HASH_LENGTH];
    if (!parse_image(image, length, &identity, hash)) {
        printf("rejected\n");
        return 0;
    }
    printf("%u %u", identity.node_id, identity.bench_only);
    for (int i = 0; i < NODE_IDENTITY_HASH_LENGTH; i++) printf(" %02x", hash[i]);
    printf("\n");
    return 0;
}
'''


def build_image(node_id=4, bench_only=0, image_magic=0xE9, desc_magic=0xABCD5432,
                identity_magic=0x42414A41, length=300):
    image = bytearray(length)
    image[0] = image_magic
    struct.pack_into("<I", image, 32, desc_magic)
    for i in range(5):
        image[32 + 144 + i] = 0xA0 + i
    if length >= 288 + 8:
        struct.pack_into("<IBBH", image, 288, identity_magic, node_id, bench_only, 0x1234)
    return bytes(image)


class ParseImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.temp.name)
        (cls.dir / "test.c").write_text(HARNESS)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT.parent),
                        str(cls.dir / "test.c"), "-o", str(cls.dir / "test")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, image):
        path = self.dir / "image.bin"
        path.write_bytes(image)
        return subprocess.check_output([str(self.dir / "test"), str(path)], text=True).strip()

    def test_valid_image(self):
        self.assertEqual(self.parse(build_image()), "4 0 a0 a1 a2 a3 a4")

    def test_bench_only_flag(self):
        self.assertEqual(self.parse(build_image(node_id=5, bench_only=1)), "5 1 a0 a1 a2 a3 a4")

    def test_master_node_id(self):
        self.assertEqual(self.parse(build_image(node_id=0)), "0 0 a0 a1 a2 a3 a4")

    def test_bad_image_magic(self):
        self.assertEqual(self.parse(build_image(image_magic=0x00)), "rejected")

    def test_bad_app_desc_magic(self):
        self.assertEqual(self.parse(build_image(desc_magic=0)), "rejected")

    def test_bad_identity_magic(self):
        self.assertEqual(self.parse(build_image(identity_magic=0)), "rejected")

    def test_short_header(self):
        self.assertEqual(self.parse(build_image(length=295)), "rejected")


if __name__ == "__main__":
    unittest.main()
