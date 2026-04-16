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

   This sketch verifies the pulse count for each gain setting by counting
   SCLK toggles during a read and printing the results. Connect a second
   GPIO to SCLK to count pulses, or just review the serial output against
   the tables above.

   Wiring for verification:
     - DOUT, SCLK, PDWN connected to ADS1232 as normal
     - MONITOR_PIN connected to SCLK via a jumper wire (same net)
     - Serial monitor at 115200 baud
*/

#include <Arduino.h>
#include <ADS1232_ADC.h>

// -- Pin configuration (adjust to your board) --
#define DOUT_PIN    11
#define SCLK_PIN    12
#define PDWN_PIN    13
#define MONITOR_PIN 14   // connect this to SCLK to count pulses

ADS1232_ADC scale(DOUT_PIN, SCLK_PIN, PDWN_PIN);

// Expected pulse counts per gain setting, derived from the upstream
// HX711_ADC mapping:
//   gain 128 → GAIN=1 → 25 pulses → ADS1232 channel 1
//   gain  32 → GAIN=2 → 26 pulses → ADS1232 channel 2
//   gain  64 → GAIN=3 → 27 pulses → (no ADS1232 meaning, rolls to ch1)
struct GainTest {
    uint8_t gain;
    uint8_t expectedPulses;
    const char* note;
};

static const GainTest tests[] = {
    {128, 25, "channel 1 (default, recommended)"},
    {  1, 26, "channel 2 (current code: any non-64/128 maps to 2 extra)"},
    { 64, 26, "BUG: upstream sends 27 (GAIN=3), refactored code sends 26"},
    { 32, 26, "channel 2 (matches upstream)"},
};
static const int NUM_TESTS = sizeof(tests) / sizeof(tests[0]);

// Count rising edges on MONITOR_PIN during one ADC read cycle.
// This is a rough software counter — good enough to distinguish
// 25 vs 26 vs 27 at ADS1232's ~10 SPS data rate.
volatile uint32_t pulseCount = 0;

void IRAM_ATTR onSclkRise() {
    pulseCount++;
}

// Wait for DOUT to go LOW (data ready), with timeout
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
    Serial.println("Expected (ADS1232 datasheet):");
    Serial.println("  25 pulses → next conversion on channel 1");
    Serial.println("  26 pulses → next conversion on channel 2");
    Serial.println("  27 pulses → no defined behavior (ADS1232 ignores)");
    Serial.println();

    // Set up pulse counter on MONITOR_PIN
    pinMode(MONITOR_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(MONITOR_PIN), onSclkRise, RISING);

    scale.begin();
    scale.start(2000, false);  // 2s stabilization, no tare

    Serial.println("Gain | Expected | Measured | Status");
    Serial.println("-----|----------|----------|-------");

    bool allPass = true;

    for (int t = 0; t < NUM_TESTS; t++) {
        scale.setGain(tests[t].gain);

        // Do a throwaway read to apply the new gain setting
        if (waitForDataReady(500)) {
            pulseCount = 0;
            scale.update();
            // discard — this read used the PREVIOUS gain's pulse count
        }

        // Now read with the new gain active
        if (!waitForDataReady(500)) {
            Serial.printf("%4d |    %2d    |  TIMEOUT | FAIL  (%s)\n",
                          tests[t].gain, tests[t].expectedPulses, tests[t].note);
            allPass = false;
            continue;
        }

        pulseCount = 0;
        scale.update();  // triggers _readADCRaw → bit-bangs SCLK
        uint32_t measured = pulseCount;

        bool pass = (measured == tests[t].expectedPulses);
        if (!pass) allPass = false;

        Serial.printf("%4d |    %2d    |    %2d    | %s  (%s)\n",
                      tests[t].gain,
                      tests[t].expectedPulses,
                      measured,
                      pass ? "OK  " : "FAIL",
                      tests[t].note);
    }

    Serial.println();
    if (allPass) {
        Serial.println("All tests passed.");
    } else {
        Serial.println("Some tests failed — see notes above.");
        Serial.println("If gain 64 shows 26 instead of 27, this confirms the");
        Serial.println("known divergence from upstream openscale (GAIN=3 vs 2).");
    }

    Serial.println();
    Serial.println("=== Recommendation ===");
    Serial.println("Use gain 128 (25 pulses, channel 1). This is the only");
    Serial.println("setting verified in production on the Half Decent Scale.");
    Serial.println("Gain on ADS1232 is set via GAIN0/GAIN1 pins, not SCLK.");
}

void loop() {
    // Nothing — one-shot verification
    delay(10000);
}
