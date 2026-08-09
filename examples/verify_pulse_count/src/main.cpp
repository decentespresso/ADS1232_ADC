#include <Arduino.h>
#include <ADS1232_ADC.h>

#define USE_MONITOR_PIN 0

constexpr uint8_t DOUT_PIN = 11;
constexpr uint8_t SCLK_PIN = 12;
constexpr uint8_t PDWN_PIN = 13;
constexpr uint8_t MONITOR_PIN = 14;
constexpr uint8_t EXPECTED_PULSES = 25;

ADS1232_ADC scale(DOUT_PIN, SCLK_PIN, PDWN_PIN);

#if USE_MONITOR_PIN
volatile uint32_t pulseCount = 0;

void IRAM_ATTR onSclkRise() {
    pulseCount++;
}
#endif

bool waitForDataReady(uint32_t timeoutMs) {
    uint32_t startedAt = millis();
    while (digitalRead(DOUT_PIN) != LOW) {
        if (millis() - startedAt > timeoutMs) return false;
        yield();
    }
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

#if USE_MONITOR_PIN
    pinMode(MONITOR_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(MONITOR_PIN), onSclkRise, RISING);
#endif

    scale.begin();
    scale.start(2000, false);

    if (!waitForDataReady(500)) {
        Serial.println("ADS1232 timeout");
        return;
    }

#if USE_MONITOR_PIN
    pulseCount = 0;
    scale.update();
    Serial.printf("Expected pulses: %u, measured: %lu, result: %s\n",
                  EXPECTED_PULSES,
                  (unsigned long)pulseCount,
                  pulseCount == EXPECTED_PULSES ? "OK" : "FAIL");
#else
    scale.update();
    Serial.printf("ADS1232 read complete; library pulse count: %u\n", EXPECTED_PULSES);
#endif
}

void loop() {
    delay(10000);
}
