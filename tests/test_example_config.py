from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO = ROOT / "examples" / "verify_pulse_count" / "platformio.ini"
README = ROOT / "README.md"
HEADER = ROOT / "include" / "ADS1232_ADC.h"
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
        self.assertIn("Compatibility overload; gain argument is ignored", readme)
        self.assertIn("Compatibility no-op; ADS1232 gain is hardware-controlled", readme)
        self.assertIn("constexpr uint8_t EXPECTED_PULSES = 25;", example)
        self.assertNotIn("GainTest", example)
        self.assertNotIn("scale.setGain", example)
        self.assertNotIn("gain 64", example.lower())

    def test_docs_describe_diagnostics_execution_context(self):
        readme = README.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")

        self.assertIn("runs synchronously after each successful read", readme)
        self.assertIn("Latest interval between successful samples in ms", readme)
        self.assertNotIn("Latest bit-bang conversion time in ms", readme)
        self.assertIn("setDebugEnabled(bool)", readme)
        self.assertIn("registered and debugging is enabled", readme)
        self.assertIn("state mutex is released before invocation", readme)
        self.assertIn("ADC I/O transaction remains locked until the callback returns", readme)
        self.assertIn("Do not call `powerDown()`, `powerUp()`, or `setChannelInUse()`", readme)
        self.assertNotIn("all library locks are released", readme)
        self.assertIn("When enabled, runs synchronously after each successful read", header)
        self.assertIn("state mutex is released before invocation", header)
        self.assertIn("ADC I/O remains locked until return", header)
        self.assertIn("do not call hardware-control methods", header)
        self.assertNotIn("library locks are released", header)
        self.assertIn("Latest interval between successful samples", header)
        self.assertNotIn("fires from FreeRTOS sampling task", header)


if __name__ == "__main__":
    unittest.main()
