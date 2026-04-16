/*
   -------------------------------------------------------------------------------------
   ADS1232_ADC
   Thread-safe Arduino library for ADS1232 24-Bit Analog-to-Digital Converter
   Refactored for FreeRTOS and Multi-Instance Support
   -------------------------------------------------------------------------------------
*/

#include <Arduino.h>
#include "ADS1232_ADC.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ADS1232_ADC::ADS1232_ADC(uint8_t dout, uint8_t sck, uint8_t pdwn, uint8_t a0, 
                         int samples, bool ignHigh, bool ignLow)
    : _dout(dout), _sck(sck), _pdwn(pdwn), _a0(a0)
{
    _maxSamples = min(samples, ADS1232_BUFFER_SIZE);
    _ignHigh = ignHigh;
    _ignLow = ignLow;
    _gain = 128;
    _samplesInUse = _maxSamples;
    _validSamples = 0;
    
    // Initialize the buffer with zeros
    for (int i = 0; i < ADS1232_BUFFER_SIZE; i++) {
        _dataBuffer[i] = 0;
    }
    _bufferIdx = 0;

    // Create the mutex for thread safety
    _mutex = xSemaphoreCreateMutex();
}

// ---------------------------------------------------------------------------
// Lifecycle: begin()
// ---------------------------------------------------------------------------
void ADS1232_ADC::begin() {
    pinMode(_dout, INPUT);
    pinMode(_sck, OUTPUT);
    pinMode(_pdwn, OUTPUT);
    
    if (_a0 != 255) {
        pinMode(_a0, OUTPUT);
    }
    
    powerUp();
    setGain(128);
}

void ADS1232_ADC::begin(uint8_t gain) {
    pinMode(_dout, INPUT);
    pinMode(_sck, OUTPUT);
    pinMode(_pdwn, OUTPUT);
    
    if (_a0 != 255) {
        pinMode(_a0, OUTPUT);
    }
    
    powerUp();
    setGain(gain);
}

// ---------------------------------------------------------------------------
// Lifecycle: beginTask() - Starts the background FreeRTOS sampling task
// ---------------------------------------------------------------------------
void ADS1232_ADC::beginTask(uint32_t intervalMs) {
    if (_taskHandle != NULL) {
        // Task already running
        return;
    }

    _taskIntervalMs = intervalMs;  // Store the interval for the task to use
    _taskRunning = true;

    // Create the FreeRTOS task
    xTaskCreatePinnedToCore(
        [](void* pvParameters) { 
            static_cast<ADS1232_ADC*>(pvParameters)->_samplingTask(NULL); 
        },
        "ADS1232_Task",
        4096,               // Stack size
        this,               // Pass 'this' pointer as parameter
        2,                  // Priority
        &_taskHandle,       // Task handle
        1                   // Core 1 (keep UI on core 0)
    );
}

// ---------------------------------------------------------------------------
// Lifecycle: end() - Stops the task and cleans up resources
// ---------------------------------------------------------------------------
void ADS1232_ADC::end() {
    // Signal the task to stop gracefully
    _taskRunning = false;

    if (_taskHandle != NULL) {
        // Give the task time to stop
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(_taskHandle);
        _taskHandle = NULL;
    }

    if (_mutex != NULL) {
        vSemaphoreDelete(_mutex);
        _mutex = NULL;
    }
}

// ---------------------------------------------------------------------------
// start() - Start ADC with stabilization and optional tare
// ---------------------------------------------------------------------------
void ADS1232_ADC::start(unsigned long t) {
    start(t, true); // Default to tare
}

void ADS1232_ADC::start(unsigned long t, bool dotare) {
    unsigned long settleTime = t + 400; // Minimum 400ms settling time at 10SPS
    unsigned long startTime = millis();
    
    // Wait for stabilization period
    while (millis() - startTime < settleTime) {
        update();
        yield();
    }
    
    if (dotare) {
        tare();
    }
}

// ---------------------------------------------------------------------------
// Internal: _samplingTask() - The FreeRTOS background task
// ---------------------------------------------------------------------------
void ADS1232_ADC::_samplingTask(void* pvParameters) {
    TickType_t lastWakeTime;
    lastWakeTime = xTaskGetTickCount();

    while (_taskRunning) {
        // Wait for the next interval using stored interval
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(_taskIntervalMs));

        // Check if DOUT is LOW (data ready)
        if (digitalRead(_dout) == LOW) {
            _readADCRaw();
        }
    }

    // Suspend ourselves; end() will call vTaskDelete() on our handle.
    // A FreeRTOS task must never return, and self-deleting here would
    // race with vTaskDelete() in end().
    vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------
// Internal: _readADCRaw() - Bit-banging ADC read (renamed from conversion24bit)
// ---------------------------------------------------------------------------
void ADS1232_ADC::_readADCRaw() {
    unsigned long data = 0;
    uint8_t dout;

    // Read 24 data bits + send GAIN extra pulses
    for (uint8_t i = 0; i < (24 + (_gain == 128 ? 1 : (_gain == 64 ? 2 : 2))); i++) {
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        digitalWrite(_sck, LOW);
        
        if (i < 24) {
            dout = digitalRead(_dout);
            data = (data << 1) | dout;
        }
    }

    // Flip the 24th bit to correct signed magnitude
    data = data ^ 0x800000;

    // Update the data buffer with the new value
    _updateBuffer((long)data);
}

// ---------------------------------------------------------------------------
// Internal: _updateBuffer() - Updates ring buffer and running sum
// ---------------------------------------------------------------------------
void ADS1232_ADC::_updateBuffer(long newValue) {
    if (_mutex != NULL) {
        if (xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE) {
            _dataBuffer[_bufferIdx] = newValue;
            _bufferIdx = (_bufferIdx + 1) % _samplesInUse;
            if (_validSamples < _samplesInUse) _validSamples++;
            
            xSemaphoreGive(_mutex);
        }
    }
}

// ---------------------------------------------------------------------------
// Thread-Safe: getData() - Returns smoothed weight
// ---------------------------------------------------------------------------
float ADS1232_ADC::getData() {
    if (_mutex == NULL) return 0.0f;

    float result = 0.0f;
    long sum = 0;
    long high = LONG_MIN;
    long low = LONG_MAX;
    int count = 0;

    if (xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE) {
        int samplesToRead = _validSamples;

        for (int i = 0; i < samplesToRead; i++) {
            long val = _dataBuffer[i];
            sum += val;
            if (val > high) high = val;
            if (val < low) low = val;
            count++;
        }

        // Outlier rejection (only if enough samples to make it meaningful)
        if (_ignHigh && count > 2) sum -= high;
        if (_ignLow && count > 2) sum -= low;

        int effectiveCount = count;
        if (_ignHigh && count > 2) effectiveCount--;
        if (_ignLow && count > 2) effectiveCount--;

        if (effectiveCount > 0) {
            result = ((float)sum / effectiveCount) - _tareOffset;
            result *= _calFactorRecip;
        }

        xSemaphoreGive(_mutex);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Thread-Safe: tare() - Blocking tare
// ---------------------------------------------------------------------------
void ADS1232_ADC::tare() {
    tareNoDelay();
    
    // Wait for tare to complete (non-blocking wait)
    uint32_t timeout = millis() + 2000; // 2 second timeout
    while (!getTareStatus()) {
        if (millis() > timeout) break;
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// tareNoDelay() - Non-blocking tare (captures current average as offset)
// ---------------------------------------------------------------------------
void ADS1232_ADC::tareNoDelay() {
    if (_mutex == NULL) return;

    if (xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE) {
        long sum = 0;
        int count = _validSamples;
        
        for (int i = 0; i < count; i++) {
            sum += _dataBuffer[i];
        }

        if (count > 0) {
            _tareOffset = (float)sum / count;
        }

        xSemaphoreGive(_mutex);
    }
}

// ---------------------------------------------------------------------------
// getTareStatus() - Returns true if tare operation is complete
// ---------------------------------------------------------------------------
bool ADS1232_ADC::getTareStatus() {
    // Non-blocking tare is instant in this implementation
    return true;
}

// ---------------------------------------------------------------------------
// setGain()
// ---------------------------------------------------------------------------
void ADS1232_ADC::setGain(uint8_t gain) {
    _gain = gain; // Hardware gain set via pins; this is for pulse count
}

// ---------------------------------------------------------------------------
// setCalFactor() & getCalFactor()
// ---------------------------------------------------------------------------
void ADS1232_ADC::setCalFactor(float cal) {
    if (_mutex != NULL && xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE) {
        _calFactor = cal;
        _calFactorRecip = 1.0f / cal;
        xSemaphoreGive(_mutex);
    } else {
        // No mutex yet (called before begin), just set directly
        _calFactor = cal;
        _calFactorRecip = 1.0f / cal;
    }
}

float ADS1232_ADC::getCalFactor() {
    return _calFactor;
}

// ---------------------------------------------------------------------------
// Power Control
// ---------------------------------------------------------------------------
void ADS1232_ADC::powerDown() {
    digitalWrite(_pdwn, LOW);
    delayMicroseconds(1);
    digitalWrite(_sck, HIGH);
}

void ADS1232_ADC::powerUp() {
    digitalWrite(_pdwn, HIGH);
    delayMicroseconds(1);
    digitalWrite(_sck, LOW);
}

// ---------------------------------------------------------------------------
// Channel Selection
// ---------------------------------------------------------------------------
void ADS1232_ADC::setChannelInUse(int channel) {
    if (_a0 == 255) return;
    digitalWrite(_a0, channel ? HIGH : LOW);
}

int ADS1232_ADC::getChannelInUse() {
    return digitalRead(_a0) == HIGH ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Sampling Control
// ---------------------------------------------------------------------------

void ADS1232_ADC::setSamplesInUse(int samples) {
    if (samples > 0 && samples <= _maxSamples) {
        if (_mutex != NULL && xSemaphoreTake(_mutex, (TickType_t)10) == pdTRUE) {
            _samplesInUse = samples;
            for (int i = 0; i < ADS1232_BUFFER_SIZE; i++) {
                _dataBuffer[i] = 0;
            }
            _bufferIdx = 0;
            _validSamples = 0;
            xSemaphoreGive(_mutex);
        }
    }
}

uint8_t ADS1232_ADC::update() {
    // Background task handles reads — don't bit-bang concurrently
    if (_taskRunning) return 0;

    if (digitalRead(_dout) == LOW) {
        _readADCRaw();
        return 1; // Data was read
    }
    return 0; // No data available
}

bool ADS1232_ADC::refreshDataSet() {
    // Background task handles reads — don't bit-bang concurrently
    if (_taskRunning) return false;

    int targetCount = _samplesInUse + (_ignHigh ? 1 : 0) + (_ignLow ? 1 : 0);
    int currentCount = 0;
    unsigned long timeout = millis() + 5000; // 5s timeout (~50 samples at 10SPS)
    
    while (currentCount < targetCount) {
        if (millis() > timeout) return false;
        if (digitalRead(_dout) == LOW) {
            _readADCRaw();
            currentCount++;
        }
        delay(1);
    }
    return true;
}

float ADS1232_ADC::getNewCalibration(float known_mass) {
    float currentValue = getData();
    if (known_mass == 0) return _calFactor;
    
    float newCalFactor = (currentValue * _calFactor) / known_mass;
    setCalFactor(newCalFactor);
    return newCalFactor;
}

