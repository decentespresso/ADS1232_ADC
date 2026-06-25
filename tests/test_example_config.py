from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO = ROOT / "examples" / "verify_pulse_count" / "platformio.ini"
README = ROOT / "README.md"
EXAMPLE = ROOT / "examples" / "verify_pulse_count" / "src" / "main.cpp"


class ExampleConfigTests(unittest.TestCase):
    def test_example_uses_local_library_checkout(self):
        text = PLATFORMIO.read_text(encoding="utf-8")

        self.assertNotIn("https://github.com/decentespresso/ADS1232_ADC.git", text)
        self.assertIn("symlink://../..", text)

    def test_docs_describe_ads1232_gain_and_sclk_correctly(self):
        readme = README.read_text(encoding="utf-8")
        example = EXAMPLE.read_text(encoding="utf-8")

        self.assertIn("ADS1232 gain is set by the GAIN0/GAIN1 pins", readme)
        self.assertNotIn("Set gain (affects clock pulse count)", readme)
        self.assertNotIn("25 pulses -> next conversion on channel 1", example)
        self.assertNotIn("26 pulses -> next conversion on channel 2", example)
        self.assertIn("26th SCLK pulse starts offset calibration", example)


if __name__ == "__main__":
    unittest.main()
