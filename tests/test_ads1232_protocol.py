from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ADS1232_ADC.cpp"


def normalized(text):
    return re.sub(r"\s+", " ", text).strip()


def method_body(name):
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(rf"\b\w+\s+ADS1232_ADC::{name}\(", text)
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


class ADS1232ProtocolTests(unittest.TestCase):
    def test_adc_read_always_sends_one_post_read_pulse(self):
        body = normalized(method_body("_readADCRaw"))

        self.assertIn("for (uint8_t i = 0; i < 25; i++)", body)
        self.assertNotIn("_gain ==", body)

    def test_channel_change_resets_sample_state(self):
        body = normalized(method_body("setChannelInUse"))

        self.assertIn("if (_mutex != NULL && xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE)", body)
        self.assertIn("for (int i = 0; i < ADS1232_BUFFER_SIZE; i++) { _dataBuffer[i] = 0; }", body)
        self.assertIn("_bufferIdx = 0;", body)
        self.assertIn("_validSamples = 0;", body)
        self.assertIn("_lastDoutLowMillis = millis();", body)
        self.assertIn("_signalTimeoutFlag = false;", body)

    def test_unused_channel_pin_returns_minus_one(self):
        body = normalized(method_body("getChannelInUse"))

        self.assertIn("if (_a0 == 255) return -1;", body)


if __name__ == "__main__":
    unittest.main()
