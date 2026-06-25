from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ADS1232_ADC.cpp"
HEADER = ROOT / "include" / "ADS1232_ADC.h"


def normalized(text):
    return re.sub(r"\s+", " ", text).strip()


def method_body(name):
    text = SOURCE.read_text(encoding="utf-8")
    pattern = rf"(?:\b\w+\s+)?ADS1232_ADC::{name}\("
    match = re.search(pattern, text)
    if match is None:
        raise AssertionError(f"method not found: {name}")

    opening = text.index("{", match.start())
    depth = 0

    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        if text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]

    raise AssertionError(f"method body not found: {name}")


class LifecycleThreadSafetyTests(unittest.TestCase):
    def test_begin_paths_recreate_mutex_after_end(self):
        header = HEADER.read_text(encoding="utf-8")

        self.assertIn("bool _ensureMutex();", header)
        self.assertIn("if (!_ensureMutex()) return;", normalized(method_body("begin")))
        self.assertIn("if (!_ensureMutex()) return;", normalized(method_body("beginTask")))
        self.assertIn("_mutex = xSemaphoreCreateMutex();", method_body("_ensureMutex"))


if __name__ == "__main__":
    unittest.main()
