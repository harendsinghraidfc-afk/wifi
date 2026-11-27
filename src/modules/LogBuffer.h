#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class LogBuffer {
public:
    static void begin(size_t capacity = 120);
    static void write(char level, const char* format, ...) __attribute__((format(printf, 2, 3)));
    static uint32_t latestSeq();
    static void fill(JsonDocument& doc, uint32_t since, uint32_t& latest);
    static String jsonSince(uint32_t since);

private:
    static const size_t SCRATCH_MAX = 60;

    struct Entry {
        uint32_t seq;
        uint32_t ms;
        char level;
        char msg[192];
    };

    static Entry* _entries;
    static size_t _capacity;
    static size_t _head;
    static size_t _count;
    static uint32_t _seq;
    static portMUX_TYPE _mux;
    static SemaphoreHandle_t _readMutex;
    static vprintf_like_t _original;
    static Entry _scratch[SCRATCH_MAX];

    static int _hook(const char* fmt, va_list args);
    static void append(const char* line);
    static char detectLevel(const char* line);
};
