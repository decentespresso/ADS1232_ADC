from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO = ROOT / "examples" / "verify_pulse_count" / "platformio.ini"


class ExampleConfigTests(unittest.TestCase):
    def test_example_uses_local_library_checkout(self):
        text = PLATFORMIO.read_text(encoding="utf-8")

        self.assertNotIn("https://github.com/decentespresso/ADS1232_ADC.git", text)
        self.assertIn("symlink://../..", text)


if __name__ == "__main__":
    unittest.main()
