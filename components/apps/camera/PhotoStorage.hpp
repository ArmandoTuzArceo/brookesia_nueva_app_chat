#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

class PhotoStorage {
public:
    enum class State : uint8_t {
        Ready,
        Preparing,
        WaitingForFrame,
        Capturing,
        Saving,
        Saved,
        Failed,
    };

    static esp_err_t start();
    static bool requestCapture();
    static bool captureFrame(const uint8_t *frame, size_t frame_size, uint32_t width, uint32_t height);
    static State state();
    static esp_err_t lastError();
    static int lastSystemError();
    static const char *lastFilename();
};
