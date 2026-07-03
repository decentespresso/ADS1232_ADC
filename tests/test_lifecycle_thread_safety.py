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
    def test_begin_paths_recreate_mutexes_after_end(self):
        header = HEADER.read_text(encoding="utf-8")
        body = normalized(method_body("_ensureMutex"))

        self.assertIn("bool _ensureMutex();", header)
        self.assertIn("if (!_ensureMutex()) return;", normalized(method_body("begin")))
        self.assertIn("if (!_ensureMutex()) return;", normalized(method_body("beginTask")))
        self.assertIn("_mutex = xSemaphoreCreateMutex();", body)
        self.assertIn("_ioMutex = xSemaphoreCreateMutex();", body)
        self.assertIn("return _mutex != NULL && _ioMutex != NULL;", body)


    def test_end_waits_for_task_ack_before_deleting_mutexes(self):
        header = normalized(HEADER.read_text(encoding="utf-8"))
        body = normalized(method_body("end"))
        task_body = normalized(method_body("_samplingTask"))

        self.assertIn("SemaphoreHandle_t _taskStopped = NULL;", header)
        self.assertIn("xTaskNotifyGive(_taskHandle);", body)
        self.assertIn("xSemaphoreTake(_taskStopped, stopTimeout) == pdTRUE", body)
        self.assertLess(body.index("xSemaphoreTake(_taskStopped, stopTimeout)"), body.index("vTaskDelete(_taskHandle);"))
        self.assertLess(body.index("vTaskDelete(_taskHandle);"), body.index("vSemaphoreDelete(_mutex);"))
        self.assertIn("xSemaphoreGive(_taskStopped);", task_body)
        self.assertIn("ulTaskNotifyTake(pdTRUE, delayTicks);", task_body)

    def test_public_state_accessors_use_mutex(self):
        methods = [
            "getCalFactor",
            "getSamplesInUse",
            "getTareStatus",
            "setDebugCallback",
            "setDebugEnabled",
            "getDebugEnabled",
            "setSignalTimeoutMs",
            "getSignalTimeoutFlag",
            "getConversionTime",
            "getSPS",
            "getTareOffset",
            "setTareOffset",
        ]

        for name in methods:
            body = method_body(name)

            self.assertIn(
                "xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE",
                body,
                name,
            )
            self.assertIn("xSemaphoreGive(_mutex);", body, name)

    def test_new_calibration_uses_synchronized_calibration_factor_read(self):
        body = normalized(method_body("getNewCalibration"))

        self.assertIn("float calFactor = getCalFactor();", body)
        self.assertIn("if (known_mass <= 0.0f || !isfinite(known_mass)) return calFactor;", body)
        self.assertIn("if (fabsf(currentValue) < ADS1232_MIN_CALIBRATION_VALUE) return calFactor;", body)
        self.assertIn("float newCalFactor = (currentValue * calFactor) / known_mass;", body)
        self.assertIn("if (!isfinite(newCalFactor) || fabsf(newCalFactor) < ADS1232_MIN_CALIBRATION_VALUE) return calFactor;", body)
        self.assertNotIn("newCalFactor <= 0.0f", body)
        self.assertNotIn("_calFactor", body)

    def test_update_buffer_copies_debug_callback_before_invoking(self):
        body = normalized(method_body("_updateBuffer"))

        self.assertIn("DebugCallback callback = nullptr;", body)
        self.assertIn("callback = _debugCallback;", body)
        self.assertIn("if (callback != nullptr)", body)
        self.assertIn("callback(info);", body)
        self.assertNotIn("_debugCallback(info);", body)


    def test_reverse_output_bool_api_resets_sample_and_tare_state(self):
        header = normalized(HEADER.read_text(encoding="utf-8"))
        source = normalized(SOURCE.read_text(encoding="utf-8"))

        self.assertIn("void setReverseOutput(bool enabled);", header)
        self.assertIn("void ADS1232_ADC::setReverseOutput() { setReverseOutput(true); }", source)
        self.assertIn("void ADS1232_ADC::setReverseOutput(bool enabled)", source)
        self.assertIn("_resetSampleStateLocked(true);", source)
        self.assertIn("_tareOffset = 0;", normalized(method_body("_resetSampleStateLocked")))


    def test_fresh_tare_api_is_added_without_changing_tare_no_delay(self):
        header = normalized(HEADER.read_text(encoding="utf-8"))
        tare_body = normalized(method_body("tareNoDelay"))

        self.assertIn("void tareFresh();", header)
        self.assertIn("void tareFreshNoDelay();", header)
        self.assertIn("if (count > 0) { _tareOffset = (float)sum / count; _tareComplete = true; }", tare_body)
        self.assertNotIn("_resetSampleStateLocked(false);", tare_body)


if __name__ == "__main__":
    unittest.main()
