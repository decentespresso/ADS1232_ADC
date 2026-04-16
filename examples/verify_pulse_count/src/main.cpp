/*
   ADS1232_ADC — Pulse Count Verification
   =======================================

   The ADS1232 uses extra SCLK pulses (beyond the 24 data bits) to select
   the input channel for the NEXT conversion:

     Total pulses | Extra | Next channel
     -------------|-------|-------------
            25    |   1   | AINP1/AINN1  (channel 1)
            26    |   2   | AINP2/AINN2  (channel 2)

   This is different from the HX711, which uses extra pulses to set gain:

     HX711 pulses | Extra | Gain | Channel
     -------------|-------|------|--------
            25    |   1   |  128 |    A
            26    |   2   |   32 |    B
            27    |   3   |   64 |    A

   The original upstream code (from openscale, based on HX711_ADC) maps
   user-facing "gain" values to SCLK pulse counts using the HX711 table.
   On the ADS1232 these pulse counts still work — they just select the
   channel, not the gain (gain is set via the physical GAIN0/GAIN1 pins).

   This example verifies the pulse count for each gain setting by
   intercepting SCLK via a GPIO interrupt on a monitor pin. If no spare
   GPIO is available, set USE_MONITOR_PIN to 0 — the test will still run
   and report expected vs actual ADC readings, just without independent
   pulse counting.

   Pin assignments below match the Sensor Basket PCB V8.1.
   Serial monitor at 115200 baud.
*/

#include <Arduino.h>
#include <ADS1232_ADC.h>

// -- Sensor Basket V8.1 pin configuration --
#define DOUT_PIN     11
#define SCLK_PIN     12
#define PDWN_PIN     13

// Set to 1 and define MONITOR_PIN if you have a spare GPIO jumpered to SCLK.
// On Sensor Basket V8.1, GPIO 14 is ACC_PWR_CTRL — not free for monitoring.
#define USE_MONITOR_PIN 0
// #define MONITOR_PIN  14

ADS1232_ADC scale(DOUT_PIN, SCLK_PIN, PDWN_PIN);

// Expected pulse counts per gain setting.
//
// Current library code (_readADCRaw):
//   gain 128          → 24 + 1 = 25 pulses
//   gain 64           → 24 + 2 = 26 pulses
//   anything else     → 24 + 2 = 26 pulses
//
// Upstream openscale (conversion24bit):
//   gain 128 → GAIN=1 → 24 + 1 = 25 pulses
//   gain  32 → GAIN=2 → 24 + 2 = 26 pulses
//   gain  64 → GAIN=3 → 24 + 3 = 27 pulses
//
// Note: gain 64 diverges between implementations.
struct GainTest {
    uint8_t gain;
    uint8_t expectedPulses;    // what current library code sends
    uint8_t upstreamPulses;    // what openscale sends
    const char* note;
};

static const GainTest tests[] = {
    {128, 25, 25, "channel 1 — default, production-verified"},
    { 32, 26, 26, "channel 2 — matches upstream"},
    { 64, 26, 27, "DIVERGES from upstream (26 vs 27)"},
    {  1, 26, 26, "channel 2 — fallthrough case"},
};
static const int NUM_TESTS = sizeof(tests) / sizeof(tests[0]);

#if USE_MONITOR_PIN
volatile uint32_t pulseCount = 0;

void IRAM_ATTR onSclkRise() {
    pulseCount++;
}
#endif

bool waitForDataReady(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (digitalRead(DOUT_PIN) != LOW) {
        if (millis() - start > timeoutMs) return false;
        yield();
    }
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("=== ADS1232 Pulse Count Verification ===");
    Serial.println();
    Serial.println("ADS1232 datasheet:");
    Serial.println("  25 pulses -> next conversion on channel 1");
    Serial.println("  26 pulses -> next conversion on channel 2");
    Serial.println("  27 pulses -> no defined meaning");
    Serial.println();

#if USE_MONITOR_PIN
    pinMode(MONITOR_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(MONITOR_PIN), onSclkRise, RISING);
    Serial.println("Monitor pin active — will count SCLK pulses.");
#else
    Serial.println("No monitor pin — reporting expected values from code analysis.");
    Serial.println("To enable hardware counting, jumper a free GPIO to SCLK");
    Serial.println("and set USE_MONITOR_PIN to 1.");
#endif
    Serial.println();

    scale.begin();
    scale.start(2000, false);

#if USE_MONITOR_PIN
    Serial.println("Gain | Expected | Measured | Upstream | Status");
    Serial.println("-----|----------|----------|----------|-------");
#else
    Serial.println("Gain | Lib sends | Upstream | Match? | Note");
    Serial.println("-----|-----------|----------|--------|-----");
#endif

    bool allMatch = true;

    for (int t = 0; t < NUM_TESTS; t++) {
        scale.setGain(tests[t].gain);

        // Throwaway read — applies new gain for the NEXT conversion
        if (waitForDataReady(500)) {
#if USE_MONITOR_PIN
            pulseCount = 0;
#endif
            scale.update();
        }

        // Actual measured read
        if (!waitForDataReady(500)) {
            Serial.printf("%4d |  TIMEOUT — is ADS1232 connected?\n", tests[t].gain);
            allMatch = false;
            continue;
        }

#if USE_MONITOR_PIN
        pulseCount = 0;
        scale.update();
        uint32_t measured = pulseCount;
        bool pass = (measured == tests[t].expectedPulses);
        if (!pass) allMatch = false;

        Serial.printf("%4d |    %2d    |    %2d    |    %2d    | %s  (%s)\n",
                      tests[t].gain,
                      tests[t].expectedPulses,
                      measured,
                      tests[t].upstreamPulses,
                      pass ? "OK  " : "FAIL",
                      tests[t].note);
#else
        scale.update();
        bool match = (tests[t].expectedPulses == tests[t].upstreamPulses);
        if (!match) allMatch = false;

        Serial.printf("%4d |    %2d     |    %2d    |  %s  | %s\n",
                      tests[t].gain,
                      tests[t].expectedPulses,
                      tests[t].upstreamPulses,
                      match ? "yes " : " NO ",
                      tests[t].note);
#endif
    }

    Serial.println();
    if (allMatch) {
        Serial.println("All gain settings match upstream.");
    } else {
        Serial.println("Discrepancies found — see table above.");
        Serial.println("Gain 64: library sends 26 pulses, upstream sends 27.");
        Serial.println("Not a production issue (gain 128 is always used).");
    }

    Serial.println();
    Serial.println("=== Recommendation ===");
    Serial.println("Use gain 128 (25 pulses, channel 1). This is the only");
    Serial.println("setting verified in production on the Half Decent Scale.");
    Serial.println("Gain on ADS1232 is set via GAIN0/GAIN1 pins, not SCLK.");
}

void loop() {
    delay(10000);
}
