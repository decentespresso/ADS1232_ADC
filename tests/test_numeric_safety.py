from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ADS1232_ADC.cpp"


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


class NumericSafetyTests(unittest.TestCase):
    def test_constructor_clamps_samples_to_valid_buffer_range(self):
        body = normalized(method_body("ADS1232_ADC"))

        self.assertIn(
            "_maxSamples = samples < 1 ? 1 : (samples > ADS1232_BUFFER_SIZE ? ADS1232_BUFFER_SIZE : samples);",
            body,
        )


if __name__ == "__main__":
    unittest.main()
