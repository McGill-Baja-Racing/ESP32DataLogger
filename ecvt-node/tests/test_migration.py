"""Migration checks against the preserved original projects; no hardware needed."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT / "tests/fixtures/migration-baseline"
MANIFEST = json.loads((PROJECT / "tests/migration_manifest.json").read_text())


def functions(text):
    mask = re.sub(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m: " " * len(m[0]), text,
    )
    pattern = r'^(?:(?:static|extern "C")[ \t]+)?(?:inline[ \t]+)?[A-Za-z_][\w:*]*(?:[ \t]+[\w:*]+)*[ \t]+(\w+)\([^;{}]*?\)[ \t\r\n]*\{'
    result = {}
    for match in re.finditer(pattern, text, re.M):
        depth, end = 1, match.end()
        while depth:
            depth += (mask[end] == "{") - (mask[end] == "}")
            end += 1
        result[match[1]] = text[match.start():end]
    return result


def without_static(text):
    return re.sub(r"^static\s+(?:inline\s+)?", "", text)


class MigrationTests(unittest.TestCase):
    def test_preserved_fixture_hashes(self):
        hashes = json.loads((ROOT / "SHA256SUMS.json").read_text())
        for name, expected in hashes.items():
            with self.subTest(path=name):
                self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), expected)

    def test_original_controller_baseline_unchanged(self):
        source = (ROOT / MANIFEST["source"]).read_bytes()
        self.assertEqual(hashlib.sha256(source).hexdigest(), MANIFEST["baseline_sha256"])

    def test_unmodified_reference_files_match(self):
        for original, migrated in MANIFEST["copies"].items():
            if migrated in MANIFEST.get("changed_copies", {}):
                continue
            if migrated.startswith("sdkconfig."):
                # ESP-IDF rewrites generated configuration during every build;
                # byte equality is appropriate for sources, not this artifact.
                continue
            with self.subTest(path=migrated):
                self.assertEqual((ROOT / original).read_bytes(), (PROJECT / migrated).read_bytes())

    def test_unmodified_extracted_functions_preserved(self):
        original = functions((ROOT / MANIFEST["source"]).read_text())
        seen = set()
        for module, names in MANIFEST["functions"].items():
            extracted = functions((PROJECT / "src" / module).read_text())
            for name in names:
                if name in MANIFEST.get("intentional_behavior_changes", {}):
                    self.assertIn(name, extracted)
                    seen.add(name)
                    continue
                with self.subTest(function=name):
                    actual = extracted[name]
                    expected = without_static(original[name])
                    if name == "app_main":
                        # Only startup change is replacing the inline mount block
                        # with a helper preserving both success and failure paths.
                        start = expected.index("    /* --------- SD mount --------- */")
                        end = expected.index("    /* Prepare unique log path", start)
                        mount_body = expected[start:end]
                        storage = functions((PROJECT / "src/storage/sd_card.cpp").read_text())["mount_sd_card"]
                        self.assertEqual(storage, "bool mount_sd_card(void)\n{\n" + mount_body.replace("        return;", "        return false;") + "    return true;\n}")
                        expected = expected[:start] + "    if (!mount_sd_card()) {\n        return;\n    }\n\n" + expected[end:]
                    self.assertEqual(actual, expected)
                    seen.add(name)
        self.assertEqual(seen, set(original))

    @unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
    def test_controller_calculations_match_original(self):
        original_source = (ROOT / MANIFEST["source"]).read_text()
        original = functions(original_source)
        migrated = functions((PROJECT / "src/control/ecvt_controller.cpp").read_text())
        names = [
            "calculate_running_rpm_adjustment", "calculate_running_target_init_from_throttle",
            "clamp_running_target_rpm", "calculate_position_from_wheel_rpm",
            "update_transition_counter", "moving_median_reset", "moving_median_update",
        ]
        constants = "\n".join(re.findall(r"^#define (?:TARGET_|STATE_TRANSITION_|CALC_MOVING_)[^\n]+", original_source, re.M))
        median_type = re.search(r"typedef struct \{\n    float values\[.*?moving_median_filter_t;", original_source, re.S)[0]
        state = "float s_lowest_gear_motor_pos=0.0f; float s_highest_gear_motor_pos=6.9f;\n"
        code = "#include <cstdint>\n#include <cstring>\n#include <cmath>\n#include <cstdio>\n" + constants + "\n" + median_type + "\n"
        for namespace, collection in [("baseline", original), ("migrated", migrated)]:
            code += "namespace " + namespace + " {\n" + state
            code += "\n".join(without_static(collection[name]) for name in names) + "\n}\n"
        code += r'''
bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
int main() {
    unsigned checks = 0;
    for (uint32_t throttle=0; throttle<=110; ++throttle) {
        float target=baseline::calculate_running_target_init_from_throttle(throttle);
        if (!same(target,migrated::calculate_running_target_init_from_throttle(throttle))) return 1;
        for (int wheel=-10; wheel<=2000; wheel+=7) {
            float a,b,c,d;
            if (!same(baseline::calculate_position_from_wheel_rpm(wheel,target,&a,&b),
                      migrated::calculate_position_from_wheel_rpm(wheel,target,&c,&d)) ||
                !same(a,c) || !same(b,d)) return 2;
            ++checks;
        }
        for (int error=0; error<=5000; error+=13) {
            if (!same(baseline::calculate_running_rpm_adjustment(error),migrated::calculate_running_rpm_adjustment(error))) return 3;
            if (!same(baseline::clamp_running_target_rpm(error,target),migrated::clamp_running_target_rpm(error,target))) return 4;
            ++checks;
        }
    }
    uint32_t ca=0,cb=0;
    for (int i=0;i<1000;++i) {
        if (baseline::update_transition_counter(&ca,i%7<4) != migrated::update_transition_counter(&cb,i%7<4) || ca!=cb) return 5;
        ++checks;
    }
    moving_median_filter_t fa={},fb={};
    baseline::moving_median_reset(&fa,3); migrated::moving_median_reset(&fb,3);
    for (uint32_t window: {0U,1U,2U,3U,16U,20U,3U}) {
        for (unsigned i=0;i<100;++i) {
            float value=float((i*37)%103)-20.5f;
            if (!same(baseline::moving_median_update(&fa,value,window),migrated::moving_median_update(&fb,value,window))) return 6;
            ++checks;
        }
    }
    if (!same(baseline::moving_median_update(nullptr,3,3),migrated::moving_median_update(nullptr,3,3))) return 7;
    std::printf("%u differential calculation cases passed\n",checks);
}
'''
        code = "#include <initializer_list>\n" + code
        with tempfile.TemporaryDirectory(prefix="ecvt-calculations-") as tmp:
            cpp, binary = Path(tmp) / "comparison.cpp", Path(tmp) / "comparison"
            cpp.write_text(code)
            subprocess.run(["c++", "-std=c++17", "-O0", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
