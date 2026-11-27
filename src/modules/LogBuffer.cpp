#include "modules/LogBuffer.h"

#include <cstring>
#include <esp_log.h>

LogBuffer::Entry* LogBuffer::_entries = nullptr;
size_t LogBuffer::_capacity = 0;
size_t LogBuffer::_head = 0;
size_t LogBuffer::_count = 0;
uint32_t LogBuffer::_seq = 0;
portMUX_TYPE LogBuffer::_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t LogBuffer::_readMutex = nullptr;
vprintf_like_t LogBuffer::_original = nullptr;
LogBuffer::Entry LogBuffer::_scratch[LogBuffer::SCRATCH_MAX];

void LogBuffer::begin(size_t capacity) {
    if (_entries) {
        return;
    }

    _capacity = capacity;
    _entries = new Entry[capacity];
    _head = 0;
    _count = 0;
    _seq = 0;

    if (!_readMutex) {
        _readMutex = xSemaphoreCreateMutex();
    }

    _original = esp_log_set_vprintf(_hook);
}

void LogBuffer::write(char level, const char* format, ...) {
    char line[192] = {level, ' '};
    va_list args;
    va_start(args, format);
    vsnprintf(line + 2, sizeof(line) - 2, format, args);
    va_end(args);
    append(line);
    Serial.println(line);
}

int LogBuffer::_hook(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);

    char buffer[256];
    int written = vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);

    if (_original) {
        _original(fmt, args);
    }

    if (written > 0) {
        append(buffer);
    }
    return written;
}

void LogBuffer::append(const char* line) {
    if (!_entries || !line) {
        return;
    }

    // Strip trailing CR/LF.
    char clean[192];
    strncpy(clean, line, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t len = strlen(clean);
    while (len > 0 && (clean[len - 1] == '\n' || clean[len - 1] == '\r')) {
        clean[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    portENTER_CRITICAL(&_mux);
    Entry& entry = _entries[_head];
    entry.seq = ++_seq;
    entry.ms = millis();
    entry.level = detectLevel(clean);
    memcpy(entry.msg, clean, sizeof(entry.msg));
    entry.msg[sizeof(entry.msg) - 1] = '\0';
    _head = (_head + 1) % _capacity;
    if (_count < _capacity) {
        _count++;
    }
    portEXIT_CRITICAL(&_mux);
}

char LogBuffer::detectLevel(const char* line) {
    // Skip ANSI escape sequences (ESP-IDF may colourise log output).
    const char* p = line;
    while (*p == '\x1b') {
        p++;
        if (*p == '[') {
            p++;
            while (*p && !(*p >= '@' && *p <= '~')) {
                p++;
            }
            if (*p) {
                p++;
            }
        } else if (*p) {
            p++;
        }
    }

    char c = *p;
    if (c == 'E' || c == 'W' || c == 'I' || c == 'D' || c == 'V') {
        return c;
    }
    return 'I';
}

uint32_t LogBuffer::latestSeq() {
    return _seq;
}

void LogBuffer::fill(JsonDocument& doc, uint32_t since, uint32_t& latest) {
    latest = _seq;
    if (!_entries || !_readMutex) {
        return;
    }
    if (xSemaphoreTake(_readMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    size_t copied = 0;
    portENTER_CRITICAL(&_mux);
    size_t start = (_count < _capacity) ? 0 : _head;
    size_t skip = _count > SCRATCH_MAX ? _count - SCRATCH_MAX : 0;
    for (size_t i = skip; i < _count && copied < SCRATCH_MAX; i++) {
        const Entry& entry = _entries[(start + i) % _capacity];
        if (entry.seq > since) {
            _scratch[copied++] = entry;
        }
    }
    latest = copied ? _scratch[copied - 1].seq : since;
    portEXIT_CRITICAL(&_mux);

    JsonArray lines = doc["lines"].to<JsonArray>();
    for (size_t i = 0; i < copied; i++) {
        JsonObject item = lines.add<JsonObject>();
        item["seq"] = _scratch[i].seq;
        item["ms"] = _scratch[i].ms;
        char level[2] = {_scratch[i].level, '\0'};
        item["level"] = level;
        item["msg"] = _scratch[i].msg;
    }
    doc["seq"] = latest;

    xSemaphoreGive(_readMutex);
}

String LogBuffer::jsonSince(uint32_t since) {
    JsonDocument doc;
    uint32_t latest = 0;
    fill(doc, since, latest);

    String out;
    serializeJson(doc, out);
    return out;
}
