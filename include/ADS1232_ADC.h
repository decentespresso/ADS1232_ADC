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
    uint8_t update();                              // Manual update (polling mode, no-op when task running)
    bool refreshDataSet();                          // Fill dataset with new conversions (blocking, no-op when task running)
    float getNewCalibration(float known_mass);      // Calculate new calibration factor

    // Hardware control
    void powerDown();
    void powerUp();
    void setChannelInUse(int channel);
    int getChannelInUse();

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
    long _dataBuffer[ADS1232_BUFFER_SIZE];
    int _bufferIdx = 0;
    int _samplesInUse = 0;
    int _validSamples = 0;                          // Number of real ADC readings in buffer
    
    // Internal processing
    void _samplingTask(void* pvParameters);         // The FreeRTOS task function
    void _readADCRaw();                             // The bit-banging ADC reader
    void _updateBuffer(long newValue);              // Updates the running sum and buffer
};

#endif
