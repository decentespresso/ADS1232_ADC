from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ADS1232_ADC.cpp"


def method_body(name):
    text = SOURCE.read_text(encoding="utf-8")
    needle = f"void ADS1232_ADC::{name}("
    start = text.index(needle)
    opening = text.index("{", start)
    depth = 0

    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        if text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]

    raise AssertionError(f"method body not found: {name}")


def calls(body):
    return [
        (match.group(1), re.sub(r"\s+", " ", match.group(2)).strip())
        for match in re.finditer(r"\b(digitalWrite|delayMicroseconds)\(([^;]+)\);", body)
    ]


def includes_ordered(actual, expected):
    cursor = 0

    for item in expected:
        try:
            cursor = actual.index(item, cursor) + 1
        except ValueError as error:
            raise AssertionError(f"missing ordered call: {item}") from error


class DefaultStartupTests(unittest.TestCase):
    def test_power_up_uses_ads1232_pdwn_toggle_sequence(self):
        includes_ordered(
            calls(method_body("powerUp")),
            [
                ("digitalWrite", "_sck, LOW"),
                ("digitalWrite", "_pdwn, LOW"),
                ("delayMicroseconds", "10"),
                ("digitalWrite", "_pdwn, HIGH"),
                ("delayMicroseconds", "26"),
                ("digitalWrite", "_pdwn, LOW"),
                ("delayMicroseconds", "26"),
                ("digitalWrite", "_pdwn, HIGH"),
            ],
        )

    def test_tare_no_delay_only_completes_with_samples(self):
        body = method_body("tareNoDelay")

        self.assertEqual(1, body.count("_tareComplete = true;"))
        self.assertRegex(
            body,
            r"if \(count > 0\) \{[^}]*_tareOffset = \(float\)sum / count;[^}]*_tareComplete = true;",
        )

    def test_tare_timeout_marks_signal_timeout(self):
        body = method_body("tare")

        self.assertIn("_signalTimeoutFlag = true;", body)

    def test_begin_task_handles_creation_failure(self):
        body = method_body("beginTask")

        self.assertIn("BaseType_t taskStatus = xTaskCreatePinnedToCore", body)
        self.assertRegex(
            body,
            r"if \(taskStatus == pdPASS\) \{[^}]*_taskRunning = true;[^}]*\} else \{[^}]*_taskHandle = NULL;[^}]*_taskRunning = false;",
        )


if __name__ == "__main__":
    unittest.main()
