from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "ADS1232_ADC.h"
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
        header = normalized(HEADER.read_text(encoding="utf-8"))
        body = normalized(method_body("_readADCRaw"))

        self.assertIn("bool _readADCRaw();", header)
        self.assertIn("return false;", body)
        self.assertIn("for (uint8_t i = 0; i < 25; i++)", body)
        self.assertNotIn("_gain ==", body)
        self.assertIn("return true;", body)

    def test_channel_change_resets_sample_state(self):
        body = normalized(method_body("setChannelInUse"))

        self.assertIn("if (!_ensureMutex()) return;", body)
        self.assertIn("if (_mutex == NULL || xSemaphoreTake(_mutex, (TickType_t)10) != pdTRUE)", body)
        self.assertLess(body.index("xSemaphoreTake(_mutex"), body.index("digitalWrite(_a0, channel ? HIGH : LOW);"))
        self.assertIn("_resetSampleStateLocked(false);", body)
        self.assertIn("_lastConvMicros = 0;", body)
        self.assertIn("_lastDoutLowMillis = millis();", body)
        self.assertIn("_signalTimeoutFlag = false;", body)

    def test_channel_change_and_adc_read_share_io_lock(self):
        header = normalized(HEADER.read_text(encoding="utf-8"))
        read_body = normalized(method_body("_readADCRaw"))
        channel_body = normalized(method_body("setChannelInUse"))

        self.assertIn("SemaphoreHandle_t _ioMutex = NULL;", header)
        self.assertLess(read_body.index("xSemaphoreTake(_ioMutex, (TickType_t)10)"), read_body.index("unsigned long now = micros();"))
        self.assertLess(read_body.index("_updateBuffer((long)signedData, dataOutOfRange);"), read_body.rindex("xSemaphoreGive(_ioMutex);"))
        self.assertLess(channel_body.index("xSemaphoreTake(_ioMutex, (TickType_t)10)"), channel_body.index("digitalWrite(_a0, channel ? HIGH : LOW);"))
        self.assertLess(channel_body.index("_signalTimeoutFlag = false;"), channel_body.rindex("xSemaphoreGive(_ioMutex);"))


    def test_power_operations_share_io_lock_with_reads(self):
        power_down = normalized(method_body("powerDown"))
        power_up = normalized(method_body("powerUp"))

        self.assertLess(power_down.index("xSemaphoreTake(_ioMutex, (TickType_t)10)"), power_down.index("digitalWrite(_pdwn, LOW);"))
        self.assertLess(power_down.index("digitalWrite(_sck, HIGH);"), power_down.rindex("xSemaphoreGive(_ioMutex);"))
        self.assertLess(power_up.index("xSemaphoreTake(_ioMutex, (TickType_t)10)"), power_up.index("digitalWrite(_sck, LOW);"))
        self.assertIn("_resetSampleStateLocked(false);", power_up)
        self.assertLess(power_up.index("_resetSampleStateLocked(false);"), power_up.rindex("xSemaphoreGive(_ioMutex);"))

    def test_callers_only_report_reads_after_io_lock_success(self):
        task_body = normalized(method_body("_samplingTask"))
        update_body = normalized(method_body("update"))
        refresh_body = normalized(method_body("refreshDataSet"))

        self.assertIn("if (digitalRead(_dout) == LOW && _readADCRaw())", task_body)
        self.assertIn("if (digitalRead(_dout) == LOW && _readADCRaw())", update_body)
        self.assertLess(update_body.index("_readADCRaw()"), update_body.index("return 1;"))
        self.assertIn("if (digitalRead(_dout) == LOW && _readADCRaw())", refresh_body)
        self.assertLess(refresh_body.index("_readADCRaw()"), refresh_body.index("currentCount++;"))

    def test_refresh_dataset_clears_timeout_after_successful_read(self):
        refresh_body = normalized(method_body("refreshDataSet"))

        self.assertLess(refresh_body.index("_readADCRaw()"), refresh_body.index("_lastDoutLowMillis = millis();"))
        self.assertLess(refresh_body.index("_lastDoutLowMillis = millis();"), refresh_body.index("_signalTimeoutFlag = false;"))
        self.assertLess(refresh_body.index("_signalTimeoutFlag = false;"), refresh_body.index("currentCount++;"))


    def test_refresh_dataset_uses_scaled_timeout(self):
        refresh_body = normalized(method_body("refreshDataSet"))
        helper_body = normalized(method_body("_refreshTimeoutForCount"))

        self.assertIn("targetCount = _samplesInUse;", refresh_body)
        self.assertNotIn("_ignHigh", refresh_body)
        self.assertNotIn("_ignLow", refresh_body)
        self.assertIn("unsigned long timeoutMs = _refreshTimeoutForCount(targetCount);", refresh_body)
        self.assertIn("millis() - startedAt > timeoutMs", refresh_body)
        self.assertIn("ADS1232_DEFAULT_CONVERSION_MS", helper_body)
        self.assertIn("ADS1232_REFRESH_TIMEOUT_MARGIN_MS", helper_body)
        self.assertNotIn("+ 5000", refresh_body)



    def test_adc_raw_values_are_sign_extended(self):
        read_body = normalized(method_body("_readADCRaw"))
        update_body = normalized(method_body("_updateBuffer"))

        self.assertIn("int32_t signedData = (data & 0x800000UL) ? (int32_t)(data | 0xFF000000UL) : (int32_t)data;", read_body)
        self.assertIn("_updateBuffer((long)signedData, dataOutOfRange);", read_body)
        self.assertNotIn("data = data ^ 0x800000", read_body)
        self.assertIn("newValue = -newValue;", update_body)
        self.assertNotIn("0xFFFFFF - newValue", update_body)

    def test_data_out_of_range_tracks_adc_saturation_codes(self):
        read_body = normalized(method_body("_readADCRaw"))
        update_body = normalized(method_body("_updateBuffer"))
        debug_body = normalized(method_body("_captureDebugInfoLocked"))

        self.assertIn("bool dataOutOfRange = (data == 0x7FFFFF || data == 0x800000);", read_body)
        self.assertIn("_lastDataOutOfRange = dataOutOfRange;", update_body)
        self.assertIn("info.dataOutOfRange = _lastDataOutOfRange;", debug_body)
        self.assertNotIn("_lastRawValue > 0xFFFFFF", debug_body)

    def test_debug_snapshot_reports_conversion_freshness(self):
        header = normalized(HEADER.read_text(encoding="utf-8"))
        update_body = normalized(method_body("_updateBuffer"))
        debug_body = normalized(method_body("_captureDebugInfoLocked"))

        self.assertIn("uint32_t conversionSequence;", header)
        self.assertIn("unsigned long lastConversionTimestamp;", header)
        self.assertIn("int validSamples;", header)
        self.assertIn("_conversionSequence++;", update_body)
        self.assertIn("_lastConversionTimestamp = millis();", update_body)
        self.assertIn("info.conversionSequence = _conversionSequence;", debug_body)
        self.assertIn("info.lastConversionTimestamp = _lastConversionTimestamp;", debug_body)
        self.assertIn("info.validSamples = _validSamples;", debug_body)

    def test_unused_channel_pin_returns_minus_one(self):
        body = normalized(method_body("getChannelInUse"))

        self.assertIn("if (_a0 == 255) return -1;", body)


if __name__ == "__main__":
    unittest.main()
