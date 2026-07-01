from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "ADS1232_ADC.h"


def default_signal_timeout_ms():
    text = HEADER.read_text(encoding="utf-8")
    match = re.search(r"#define\s+DEFAULT_SIGNAL_TIMEOUT_MS\s+(\d+)", text)
    if match is None:
        raise AssertionError("DEFAULT_SIGNAL_TIMEOUT_MS not found")
    return int(match.group(1))


class SignalTimeoutDefaultTests(unittest.TestCase):
    def test_default_signal_timeout_allows_10_sps_conversion_jitter(self):
        self.assertGreaterEqual(default_signal_timeout_ms(), 250)


if __name__ == "__main__":
    unittest.main()
