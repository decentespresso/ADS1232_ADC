/*
   -------------------------------------------------------------------------------------
   ADS1232_ADC
   Arduino library for ADS1232 24-Bit Analog-to-Digital Converter for Weight Scales
   By Sofronio Chen July2024
   Based on HX711_ADC By Olav Kallhovd sept2017
   -------------------------------------------------------------------------------------
*/

#ifndef ADS1232_ADC_h
#define ADS1232_ADC_h

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Default max samples constant for compatibility
#define MAX_SAMPLES 64
#define ADS1232_BUFFER_SIZE 256
#define DEFAULT_SIGNAL_TIMEOUT_MS 100

// Debug info structure — lightweight snapshot, no statistics precomputed.
struct ADS1232DebugInfo {
    unsigned long timestamp;        // millis() when debug info was captured
    long rawValue;                  // Latest raw 24-bit value read (after reverse)
    long smoothedValue;             // Smoothed value after filtering
    long tareOffset;                // Current tare offset
    float conversionTimeMs;         // Latest conversion time in ms
    float sps;                      // Samples per second
    int readIndex;                  // Current buffer index
    int samplesInUse;               // Number of samples being averaged
    int validSamples;               // Number of real ADC readings in buffer
    bool dataOutOfRange;            // If data exceeded valid 24-bit range
    bool signalTimeout;             // If DOUT signal timed out
};

// Debug callback — fires from FreeRTOS sampling task, once per conversion.
// Must be fast (< 1ms), non-blocking, no mutex acquisition.
// Gets a snapshot copy; calling getData() from the callback is safe.
typedef void (*DebugCallback)(const ADS1232DebugInfo& info);

class ADS1232_ADC {
public:
    // Constructor: Defines pins and configuration at runtime
    ADS1232_ADC(uint8_t dout, uint8_t sck, uint8_t pdwn, uint8_t a0 = 255,
                int samples = 64, bool ignHigh = true, bool ignLow = true);

    // Lifecycle
    void begin();                                   // Initialize pins and power up
    void begin(uint8_t gain);                       // Initialize with specific gain
    void beginTask(uint32_t intervalMs);             // Starts the background FreeRTOS sampling task
    void end();                                     // Stops task and cleans up resources
    void start(unsigned long t);                     // Start ADC and do tare
    void start(unsigned long t, bool dotare);        // Start ADC, tare if selected

    // Thread-Safe API
    void setGain(uint8_t gain = 128);               // 32, 64, or 128
    void setCalFactor(float cal);                   // Set new calibration factor
    float getCalFactor();                           // Get current calibration factor
    float getData();                                // Thread-safe: returns smoothed weight
    void tare();                                    // Blocking tare
    void tareNoDelay();                             // Non-blocking tare
    bool getTareStatus();                           // Check if tare-in-progress is complete

    // Sampling control
    void setSamplesInUse(int samples);              // Set averaging window (1 = fast, MAX_SAMPLES = smooth)
    int getSamplesInUse();                          // Get current samples in use
    uint8_t update();                              // Manual update (polling mode, no-op when task running)
    bool refreshDataSet();                          // Fill dataset with new conversions (blocking, no-op when task running)
    float getNewCalibration(float known_mass);      // Calculate new calibration factor

    // Hardware control
    void powerDown();
    void powerUp();
    void setChannelInUse(int channel);
    int getChannelInUse();
    uint8_t getDoutPin();                           // Returns DOUT pin number

    // Debug & diagnostics
    void setDebugCallback(DebugCallback callback);  // Fire callback on each conversion
    void setDebugEnabled(bool enabled);             // Enable/disable debug callbacks
    bool getDebugEnabled();                         // Check if debug is enabled
    ADS1232DebugInfo getDebugInfo();                // Snapshot of current state
    void setSignalTimeoutMs(uint32_t ms);           // Override DOUT timeout (default 100ms)
    bool getSignalTimeoutFlag();                    // True if DOUT inactive > timeout

    // Conversion timing diagnostics
    float getConversionTime();                      // Latest conversion time in ms
    float getSPS();                                 // Samples per second from latest conversion
    float getSettlingTime();                        // Estimated settling time = conversionTime * samplesInUse

    // Raw offset access (for calibration tools)
    long getTareOffset();                           // Get raw tare offset
    void setTareOffset(long newoffset);             // Set raw tare offset

    // Output control
    void setReverseOutput();                        // Flip sign of all output values

private:
    // Hardware Pins
    uint8_t _dout, _sck, _pdwn, _a0;
    uint8_t _gain;

    // Configuration properties
    int _maxSamples;
    bool _ignHigh;
    bool _ignLow;
    uint32_t _taskIntervalMs = 100;  // Store the sampling interval

    // Threading & Sync
    TaskHandle_t _taskHandle = NULL;
    SemaphoreHandle_t _mutex = NULL;
    volatile bool _taskRunning = false;  // Flag to signal task to stop

    // Data Storage
    float _calFactor = 1.0;
    float _calFactorRecip = 1.0;
    float _tareOffset = 0;
    volatile bool _tareComplete = false;            // One-shot flag: true after tare, cleared on read
    long _dataBuffer[ADS1232_BUFFER_SIZE];
    int _bufferIdx = 0;
    int _samplesInUse = 0;
    int _validSamples = 0;                          // Number of real ADC readings in buffer

    // Debug & diagnostics
    DebugCallback _debugCallback = nullptr;
    bool _debugEnabled = false;
    volatile long _lastRawValue = 0;
    volatile unsigned long _conversionTimeMicros = 0;  // interval between successive conversions (true sample period)
    volatile unsigned long _lastConvMicros = 0;        // micros() of previous conversion; 0 = no prior sample
    volatile unsigned long _lastDoutLowMillis = 0;
    volatile bool _signalTimeoutFlag = false;
    uint32_t _signalTimeoutMs = DEFAULT_SIGNAL_TIMEOUT_MS;
    bool _reverseVal = false;

    // Internal processing
    void _samplingTask(void* pvParameters);         // The FreeRTOS task function
    void _readADCRaw();                             // The bit-banging ADC reader
    void _updateBuffer(long newValue);              // Updates the running sum and buffer
    ADS1232DebugInfo _captureDebugInfoLocked();     // Snapshot under mutex
};

#endif
