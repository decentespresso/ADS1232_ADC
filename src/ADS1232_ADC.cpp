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
    _maxSamples = samples < 1 ? 1 : (samples > ADS1232_BUFFER_SIZE ? ADS1232_BUFFER_SIZE : samples);
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
    _ioMutex = xSemaphoreCreateMutex();
}

ADS1232_ADC::~ADS1232_ADC() {
    end();
}

bool ADS1232_ADC::_ensureMutex() {
    if (_mutex == NULL) {
        _mutex = xSemaphoreCreateMutex();
    }

    if (_ioMutex == NULL) {
        _ioMutex = xSemaphoreCreateMutex();
    }

    return _mutex != NULL && _ioMutex != NULL;
}

void ADS1232_ADC::_resetSampleStateLocked(bool resetTareOffset) {
    for (int i = 0; i < ADS1232_BUFFER_SIZE; i++) {
        _dataBuffer[i] = 0;
    }
    _bufferIdx = 0;
    _validSamples = 0;
    _tareComplete = false;
    _tareFreshPending = false;
    _tareFreshSamplesNeeded = 0;
    _lastDataOutOfRange = false;
    if (resetTareOffset) {
        _tareOffset = 0;
    }
}

float ADS1232_ADC::_filteredAverageLocked() {
    int64_t sum = 0;
    long high = LONG_MIN;
    long low = LONG_MAX;

    for (int i = 0; i < _validSamples; i++) {
        long value = _dataBuffer[i];
        sum += value;
        if (value > high) high = value;
        if (value < low) low = value;
    }

    int count = _validSamples;
    if (_ignHigh && _validSamples > 2) {
        sum -= high;
        count--;
    }
    if (_ignLow && _validSamples > 2) {
        sum -= low;
        count--;
    }

    return count > 0 ? (float)sum / count : 0.0f;
}

void ADS1232_ADC::_commitFreshTareIfReadyLocked() {
    if (!_tareFreshPending || _validSamples < _tareFreshSamplesNeeded) return;

    _tareOffset = _filteredAverageLocked();
    _tareFreshPending = false;
    _tareFreshSamplesNeeded = 0;
    _tareComplete = true;
}

unsigned long ADS1232_ADC::_refreshTimeoutForCount(int targetCount) {
    unsigned long conversionTimeMs = ADS1232_DEFAULT_CONVERSION_MS;

    if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (_conversionTimeMicros > 0) {
            conversionTimeMs = (_conversionTimeMicros + 999) / 1000;
        }
        xSemaphoreGive(_mutex);
    }

    if (conversionTimeMs < 1) {
        conversionTimeMs = 1;
    }

    unsigned long samplesToWaitFor = targetCount > 0 ? (unsigned long)targetCount : 1;
    return ((samplesToWaitFor + 2) * conversionTimeMs) + ADS1232_REFRESH_TIMEOUT_MARGIN_MS;
}

// ---------------------------------------------------------------------------
// Lifecycle: begin()
// ---------------------------------------------------------------------------
void ADS1232_ADC::begin() {
    if (!_ensureMutex()) return;

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
    if (!_ensureMutex()) return;

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
    if (!_ensureMutex()) return;

    if (_taskHandle != NULL) {
        // Task already running
        return;
    }

    if (_taskStopped != NULL) {
        vSemaphoreDelete(_taskStopped);
        _taskStopped = NULL;
    }

    _taskIntervalMs = intervalMs < ADS1232_MIN_TASK_INTERVAL_MS ? ADS1232_MIN_TASK_INTERVAL_MS : intervalMs;
    _lastDoutLowMillis = millis();
    _taskStopped = xSemaphoreCreateBinary();
    if (_taskStopped == NULL) return;

    _taskRunning.store(true);

    BaseType_t taskStatus = xTaskCreate(
        [](void* pvParameters) {
            ADS1232_ADC* instance = static_cast<ADS1232_ADC*>(pvParameters);
            instance->_samplingTask(NULL);
        },
        "ADS1232_Task",
        4096,
        this,
        2,
        &_taskHandle
    );

    if (taskStatus != pdPASS) {
        _taskHandle = NULL;
        _taskRunning.store(false);
        vSemaphoreDelete(_taskStopped);
        _taskStopped = NULL;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle: end() - Stops the task and cleans up resources
// ---------------------------------------------------------------------------
void ADS1232_ADC::end() {
    _taskRunning.store(false);

    if (_taskHandle != NULL) {
        xTaskNotifyGive(_taskHandle);
        TickType_t stopTimeout = pdMS_TO_TICKS(_taskIntervalMs + 100);
        if (stopTimeout < pdMS_TO_TICKS(100)) {
            stopTimeout = pdMS_TO_TICKS(100);
        }
        if (_taskStopped != NULL && xSemaphoreTake(_taskStopped, stopTimeout) == pdTRUE) {
            vTaskDelete(_taskHandle);
            _taskHandle = NULL;
        }
    }

    if (_taskHandle == NULL) {
        if (_taskStopped != NULL) {
            vSemaphoreDelete(_taskStopped);
            _taskStopped = NULL;
        }

        if (_mutex != NULL) {
            vSemaphoreDelete(_mutex);
            _mutex = NULL;
        }

        if (_ioMutex != NULL) {
            vSemaphoreDelete(_ioMutex);
            _ioMutex = NULL;
        }
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
    TickType_t delayTicks = pdMS_TO_TICKS(_taskIntervalMs);
    if (delayTicks < 1) {
        delayTicks = 1;
    }

    while (_taskRunning.load()) {
        ulTaskNotifyTake(pdTRUE, delayTicks);
        if (!_taskRunning.load()) break;

        // Check if DOUT is LOW (data ready)
        if (digitalRead(_dout) != LOW || !_readADCRaw()) {
            _checkSignalTimeout();
        }
    }

    if (_taskStopped != NULL) {
        xSemaphoreGive(_taskStopped);
    }
    vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------
// Internal: _readADCRaw() - Bit-banging ADC read
// ---------------------------------------------------------------------------
bool ADS1232_ADC::_readADCRaw() {
    if (_ioMutex == NULL || xSemaphoreTake(_ioMutex, portMAX_DELAY) != pdTRUE) return false;

    if (digitalRead(_dout) != LOW) {
        xSemaphoreGive(_ioMutex);
        return false;
    }

    // Measure the interval since the previous conversion — this is the true
    // sample period (DOUT-ready to DOUT-ready), which getSPS()/getConversionTime()
    // report. Timing the read loop itself would only yield the bit-bang duration
    // (~90us → a meaningless ~11kHz), not the ADS1232's actual data rate.
    unsigned long now = micros();
    unsigned long data = 0;
    uint8_t dout;

    portENTER_CRITICAL(&_clockMux);
    for (uint8_t i = 0; i < 25; i++) {
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        digitalWrite(_sck, LOW);

        if (i < 24) {
            dout = digitalRead(_dout);
            data = (data << 1) | dout;
        }
    }
    portEXIT_CRITICAL(&_clockMux);

    bool dataOutOfRange = (data == 0x7FFFFF || data == 0x800000);
    int32_t signedData = (data & 0x800000UL) ? (int32_t)(data | 0xFF000000UL) : (int32_t)data;

    // Update the data buffer with the new value
    bool updated = _updateBuffer((long)signedData, dataOutOfRange, now);
    xSemaphoreGive(_ioMutex);
    return updated;
}

// ---------------------------------------------------------------------------
// Internal: _updateBuffer() - Updates ring buffer, fires debug callback
// ---------------------------------------------------------------------------
bool ADS1232_ADC::_updateBuffer(long newValue, bool dataOutOfRange, unsigned long conversionMicros) {
    DebugCallback callback = nullptr;
    bool updated = false;

    if (_mutex != NULL) {
        if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            // Apply reverse if enabled
            if (_reverseVal) {
                newValue = -newValue;
            }

            if (_lastConvMicros != 0) {
                _conversionTimeMicros = conversionMicros - _lastConvMicros;
            }
            _lastConvMicros = conversionMicros;
            _lastDoutLowMillis = millis();
            _signalTimeoutFlag = false;
            _dataBuffer[_bufferIdx] = newValue;
            _bufferIdx = (_bufferIdx + 1) % _samplesInUse;
            if (_validSamples < _samplesInUse) _validSamples++;
            _lastRawValue = newValue;
            _lastDataOutOfRange = dataOutOfRange;
            _commitFreshTareIfReadyLocked();

            // Capture debug snapshot under mutex, fire callback after release
            if (_debugEnabled && _debugCallback != nullptr) {
                callback = _debugCallback;
            }

            updated = true;
            xSemaphoreGive(_mutex);
        }
    }

    // Fire callback outside mutex — callback may call getData() safely
    if (callback != nullptr) {
        ADS1232DebugInfo info = getDebugInfo();
        callback(info);
    }

    return updated;
}

void ADS1232_ADC::_checkSignalTimeout() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;

    if (millis() - _lastDoutLowMillis > _signalTimeoutMs) {
        _signalTimeoutFlag = true;
    }

    xSemaphoreGive(_mutex);
}

// ---------------------------------------------------------------------------
// Thread-Safe: getData() - Returns smoothed weight
// ---------------------------------------------------------------------------
float ADS1232_ADC::getData() {
    if (_mutex == NULL) return 0.0f;

    float result = 0.0f;

    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (_validSamples > 0) {
            result = _filteredAverageLocked() - _tareOffset;
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
    // tareNoDelay is instant — captures current buffer average.
    uint32_t startedAt = millis();
    while (!getTareStatus()) {
        if (millis() - startedAt > 2000) {
            if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
                _signalTimeoutFlag = true;
                xSemaphoreGive(_mutex);
            }
            break;
        }
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// tareNoDelay() - Non-blocking tare (captures current average as offset)
// ---------------------------------------------------------------------------
void ADS1232_ADC::tareNoDelay() {
    if (_mutex == NULL) return;

    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (_validSamples > 0) {
            _tareOffset = _filteredAverageLocked();
            _tareComplete = true;
        }

        xSemaphoreGive(_mutex);
    }
}

void ADS1232_ADC::tareFresh() {
    tareFreshNoDelay();

    unsigned long startedAt = millis();
    unsigned long timeoutMs = _refreshTimeoutForCount(getSamplesInUse());
    while (!getTareStatus()) {
        if (!_taskRunning.load()) {
            update();
        }
        if (millis() - startedAt > timeoutMs) {
            if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
                _tareFreshPending = false;
                _tareFreshSamplesNeeded = 0;
                _signalTimeoutFlag = true;
                xSemaphoreGive(_mutex);
            }
            break;
        }
        delay(10);
        yield();
    }
}

void ADS1232_ADC::tareFreshNoDelay() {
    if (_mutex == NULL) return;

    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _resetSampleStateLocked(false);
        _tareFreshPending = true;
        _tareFreshSamplesNeeded = _samplesInUse > 0 ? _samplesInUse : 1;
        xSemaphoreGive(_mutex);
    }
}

// ---------------------------------------------------------------------------
// getTareStatus() - Returns true if tare operation is complete
// ---------------------------------------------------------------------------
bool ADS1232_ADC::getTareStatus() {
    // One-shot: return true once after tare completes, then false.
    // Matches openscale's expectation — prevents re-entering tare-reset.
    bool result = false;

    if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        result = _tareComplete;
        _tareComplete = false;
        xSemaphoreGive(_mutex);
    }

    return result;
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
    if (!std::isfinite(cal) || fabsf(cal) < ADS1232_MIN_CALIBRATION_VALUE) {
        return;
    }

    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;

    _calFactor = cal;
    _calFactorRecip = 1.0f / cal;
    xSemaphoreGive(_mutex);
}

float ADS1232_ADC::getCalFactor() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return 1.0f;

    float result = _calFactor;
    xSemaphoreGive(_mutex);

    return result;
}

// ---------------------------------------------------------------------------
// Power Control
// ---------------------------------------------------------------------------
void ADS1232_ADC::powerDown() {
    if (!_ensureMutex()) return;
    if (_ioMutex == NULL || xSemaphoreTake(_ioMutex, portMAX_DELAY) != pdTRUE) return;

    digitalWrite(_pdwn, LOW);
    delayMicroseconds(1);
    digitalWrite(_sck, HIGH);

    xSemaphoreGive(_ioMutex);
}

void ADS1232_ADC::powerUp() {
    if (!_ensureMutex()) return;
    if (_ioMutex == NULL || xSemaphoreTake(_ioMutex, portMAX_DELAY) != pdTRUE) return;

    digitalWrite(_sck, LOW);
    digitalWrite(_pdwn, LOW);
    delayMicroseconds(10);
    digitalWrite(_pdwn, HIGH);
    delayMicroseconds(26);
    digitalWrite(_pdwn, LOW);
    delayMicroseconds(26);
    digitalWrite(_pdwn, HIGH);

    if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _lastConvMicros = 0;
        _lastDoutLowMillis = millis();
        _signalTimeoutFlag = false;
        _resetSampleStateLocked(false);
        xSemaphoreGive(_mutex);
    }

    xSemaphoreGive(_ioMutex);
}

// ---------------------------------------------------------------------------
// Channel Selection
// ---------------------------------------------------------------------------
void ADS1232_ADC::setChannelInUse(int channel) {
    if (_a0 == 255) return;
    if (!_ensureMutex()) return;
    if (_ioMutex == NULL || xSemaphoreTake(_ioMutex, portMAX_DELAY) != pdTRUE) return;

    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(_ioMutex);
        return;
    }

    digitalWrite(_a0, channel ? HIGH : LOW);
    _resetSampleStateLocked(false);
    _lastConvMicros = 0;
    _lastDoutLowMillis = millis();
    _signalTimeoutFlag = false;

    xSemaphoreGive(_mutex);
    xSemaphoreGive(_ioMutex);
}

int ADS1232_ADC::getChannelInUse() {
    if (_a0 == 255) return -1;
    if (!_ensureMutex()) return -1;
    if (_ioMutex == NULL || xSemaphoreTake(_ioMutex, portMAX_DELAY) != pdTRUE) return -1;

    int channel = digitalRead(_a0) == HIGH ? 1 : 0;
    xSemaphoreGive(_ioMutex);
    return channel;
}

uint8_t ADS1232_ADC::getDoutPin() {
    return _dout;
}

// ---------------------------------------------------------------------------
// Sampling Control
// ---------------------------------------------------------------------------

void ADS1232_ADC::setSamplesInUse(int samples) {
    if (samples > 0 && samples <= _maxSamples) {
        if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            _samplesInUse = samples;
            _resetSampleStateLocked(false);
            xSemaphoreGive(_mutex);
        }
    }
}

int ADS1232_ADC::getSamplesInUse() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return 0;

    int result = _samplesInUse;
    xSemaphoreGive(_mutex);

    return result;
}

uint8_t ADS1232_ADC::update() {
    // Background task handles reads — don't bit-bang concurrently
    if (_taskRunning.load()) return 0;

    if (digitalRead(_dout) == LOW && _readADCRaw()) return 1;

    _checkSignalTimeout();
    return 0;
}

bool ADS1232_ADC::refreshDataSet() {
    // Background task handles reads - don't bit-bang concurrently
    if (_taskRunning.load()) return false;

    int targetCount = 0;
    if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        targetCount = _samplesInUse + (_ignHigh ? 1 : 0) + (_ignLow ? 1 : 0);
        xSemaphoreGive(_mutex);
    } else {
        return false;
    }

    int currentCount = 0;
    unsigned long startedAt = millis();
    unsigned long timeoutMs = _refreshTimeoutForCount(targetCount);

    while (currentCount < targetCount) {
        if (millis() - startedAt > timeoutMs) {
            _checkSignalTimeout();
            return false;
        }
        if (digitalRead(_dout) == LOW && _readADCRaw()) {
            currentCount++;
        }
        delay(1);
    }
    return true;
}

float ADS1232_ADC::getNewCalibration(float known_mass) {
    float currentValue = getData();
    float calFactor = getCalFactor();
    if (known_mass <= 0.0f || !std::isfinite(known_mass)) return calFactor;
    if (fabsf(currentValue) < ADS1232_MIN_CALIBRATION_VALUE) return calFactor;

    float newCalFactor = (currentValue * calFactor) / known_mass;
    if (!std::isfinite(newCalFactor) || fabsf(newCalFactor) < ADS1232_MIN_CALIBRATION_VALUE) return calFactor;
    setCalFactor(newCalFactor);
    return newCalFactor;
}

// ---------------------------------------------------------------------------
// Debug & Diagnostics
// ---------------------------------------------------------------------------

void ADS1232_ADC::setDebugCallback(DebugCallback callback) {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;

    _debugCallback = callback;
    xSemaphoreGive(_mutex);
}

void ADS1232_ADC::setDebugEnabled(bool enabled) {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;

    _debugEnabled = enabled;
    xSemaphoreGive(_mutex);
}

bool ADS1232_ADC::getDebugEnabled() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return false;

    bool result = _debugEnabled;
    xSemaphoreGive(_mutex);

    return result;
}

ADS1232DebugInfo ADS1232_ADC::getDebugInfo() {
    ADS1232DebugInfo info = {};

    if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        info = _captureDebugInfoLocked();
        xSemaphoreGive(_mutex);
    }

    return info;
}

ADS1232DebugInfo ADS1232_ADC::_captureDebugInfoLocked() {
    ADS1232DebugInfo info = {};

    info.timestamp = millis();
    info.rawValue = _lastRawValue;
    info.tareOffset = (long)_tareOffset;
    info.conversionTimeMs = _conversionTimeMicros / 1000.0f;
    info.sps = (_conversionTimeMicros > 0) ? (1000000.0f / _conversionTimeMicros) : 0.0f;
    info.readIndex = _bufferIdx;
    info.samplesInUse = _samplesInUse;
    info.validSamples = _validSamples;
    info.signalTimeout = _signalTimeoutFlag;

    if (_validSamples > 0) {
        info.smoothedValue = (long)_filteredAverageLocked();
    }

    info.dataOutOfRange = _lastDataOutOfRange;

    return info;
}

void ADS1232_ADC::setSignalTimeoutMs(uint32_t ms) {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;

    _signalTimeoutMs = ms;
    xSemaphoreGive(_mutex);
}

bool ADS1232_ADC::getSignalTimeoutFlag() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return false;

    bool result = _signalTimeoutFlag;
    xSemaphoreGive(_mutex);

    return result;
}

// ---------------------------------------------------------------------------
// Conversion Timing Diagnostics
// ---------------------------------------------------------------------------

float ADS1232_ADC::getConversionTime() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return 0.0f;

    unsigned long conversionTimeMicros = _conversionTimeMicros;
    xSemaphoreGive(_mutex);

    return conversionTimeMicros / 1000.0f;
}

float ADS1232_ADC::getSPS() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return 0.0f;

    unsigned long conversionTimeMicros = _conversionTimeMicros;
    xSemaphoreGive(_mutex);

    if (conversionTimeMicros == 0) return 0.0f;
    return 1000000.0f / conversionTimeMicros;
}

float ADS1232_ADC::getSettlingTime() {
    int samplesInUse = getSamplesInUse();
    return getConversionTime() * samplesInUse;
}

// ---------------------------------------------------------------------------
// Raw Offset Access
// ---------------------------------------------------------------------------

long ADS1232_ADC::getTareOffset() {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return 0;

    long result = (long)_tareOffset;
    xSemaphoreGive(_mutex);

    return result;
}

void ADS1232_ADC::setTareOffset(long newoffset) {
    if (_mutex == NULL || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;

    _tareOffset = (float)newoffset;
    xSemaphoreGive(_mutex);
}

// ---------------------------------------------------------------------------
// Output Control
// ---------------------------------------------------------------------------

void ADS1232_ADC::setReverseOutput() {
    setReverseOutput(true);
}

void ADS1232_ADC::setReverseOutput(bool enabled) {
    if (!_ensureMutex()) return;

    if (_mutex != NULL && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (_reverseVal != enabled) {
            _reverseVal = enabled;
            _resetSampleStateLocked(true);
        }
        xSemaphoreGive(_mutex);
    }
}
