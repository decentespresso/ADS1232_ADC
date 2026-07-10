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

    def test_sample_accumulators_are_wide_enough_for_full_buffer(self):
        self.assertGreater(0xFFFFFF * 256, 2_147_483_647)

        source = SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("long sum = 0;", source)
        self.assertEqual(1, source.count("int64_t sum = 0;"))

    def test_unsafe_calibration_factors_are_rejected_without_rejecting_sign(self):
        set_cal_body = normalized(method_body("setCalFactor"))
        new_cal_body = normalized(method_body("getNewCalibration"))

        self.assertIn("if (!std::isfinite(cal) || fabsf(cal) < ADS1232_MIN_CALIBRATION_VALUE) { return; }", set_cal_body)
        self.assertNotIn("cal <= 0.0f", set_cal_body)
        self.assertIn("if (known_mass <= 0.0f || !std::isfinite(known_mass)) return calFactor;", new_cal_body)
        self.assertIn("if (fabsf(currentValue) < ADS1232_MIN_CALIBRATION_VALUE) return calFactor;", new_cal_body)
        self.assertIn("if (!std::isfinite(newCalFactor) || fabsf(newCalFactor) < ADS1232_MIN_CALIBRATION_VALUE) return calFactor;", new_cal_body)
        self.assertNotIn("newCalFactor <= 0.0f", new_cal_body)

    def test_debug_snapshots_are_zero_initialized(self):
        source = SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("ADS1232DebugInfo info;", source)
        self.assertEqual(2, source.count("ADS1232DebugInfo info = {};"))


if __name__ == "__main__":
    unittest.main()
