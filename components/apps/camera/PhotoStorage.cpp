#include "PhotoStorage.hpp"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <atomic>

#include "bsp/esp-bsp.h"
#include "driver/jpeg_encode.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "PhotoStorage";
constexpr size_t PATH_SIZE = 256;
constexpr uint32_t JPEG_QUALITY = 80;

struct PhotoJob {
    uint8_t *frame;
    size_t frame_size;
    uint32_t width;
    uint32_t height;
};

QueueHandle_t save_queue;
TaskHandle_t save_task;
std::atomic<PhotoStorage::State> current_state{PhotoStorage::State::Ready};
std::atomic<int> current_error{ESP_OK};
std::atomic<int> current_system_error{0};
char current_filename[32] = {};

void setFailure(esp_err_t error, int system_error)
{
    current_error.store(error, std::memory_order_relaxed);
    current_system_error.store(system_error, std::memory_order_relaxed);
    current_state.store(PhotoStorage::State::Failed, std::memory_order_release);
}

esp_err_t ensureDirectory(const char *path, int *system_error)
{
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }

    if (errno != EEXIST) {
        *system_error = errno;
        return ESP_FAIL;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        *system_error = errno;
        return ESP_FAIL;
    }
    if (!S_ISDIR(info.st_mode)) {
        *system_error = ENOTDIR;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t makePhotoDirectory(char *directory, size_t directory_size, int *system_error)
{
    int written = snprintf(directory, directory_size, "%s/DCIM/Camera", BSP_SD_MOUNT_POINT);
    if (written < 0 || static_cast<size_t>(written) >= directory_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    char dcim_directory[PATH_SIZE];
    written = snprintf(dcim_directory, sizeof(dcim_directory), "%s/DCIM", BSP_SD_MOUNT_POINT);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(dcim_directory)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t result = ensureDirectory(dcim_directory, system_error);
    if (result != ESP_OK) {
        return result;
    }
    return ensureDirectory(directory, system_error);
}

esp_err_t makeNextFilename(const char *directory, char *filename, size_t filename_size, int *system_error)
{
    DIR *dir = opendir(directory);
    if (dir == nullptr) {
        *system_error = errno;
        return ESP_FAIL;
    }

    unsigned long highest_index = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char *name = entry->d_name;
        if (strncmp(name, "ESP_", 4) != 0) {
            continue;
        }

        char *end = nullptr;
        unsigned long index = strtoul(name + 4, &end, 10);
        if (end == name + 4 || strcasecmp(end, ".jpg") != 0) {
            continue;
        }
        bool valid_index = true;
        for (const char *digit = name + 4; digit < end; ++digit) {
            if (*digit < '0' || *digit > '9') {
                valid_index = false;
                break;
            }
        }
        if (!valid_index) {
            continue;
        }
        if (index > highest_index) {
            highest_index = index;
        }
    }

    int close_result = closedir(dir);
    if (close_result != 0) {
        *system_error = errno;
        return ESP_FAIL;
    }
    if (highest_index == ULONG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written = snprintf(filename, filename_size, "ESP_%06lu.jpg", highest_index + 1);
    if (written < 0 || static_cast<size_t>(written) >= filename_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t saveJpeg(const PhotoJob &job, char *filename, size_t filename_size, int *system_error)
{
    char directory[PATH_SIZE];
    esp_err_t result = makePhotoDirectory(directory, sizeof(directory), system_error);
    if (result != ESP_OK) {
        return result;
    }

    result = makeNextFilename(directory, filename, filename_size, system_error);
    if (result != ESP_OK) {
        return result;
    }

    char final_path[PATH_SIZE];
    char temporary_path[PATH_SIZE];
    int written = snprintf(final_path, sizeof(final_path), "%s/%s", directory, filename);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(final_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(temporary_path, sizeof(temporary_path), "%s/%s.tmp", directory, filename);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(temporary_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (job.frame_size > UINT32_MAX / 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t output_capacity = job.frame_size * 2;

    jpeg_encode_memory_alloc_cfg_t output_memory_config = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_buffer_size = 0;
    uint8_t *output_buffer = static_cast<uint8_t *>(
        jpeg_alloc_encoder_mem(output_capacity, &output_memory_config, &output_buffer_size));
    if (output_buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (output_buffer_size > UINT32_MAX) {
        free(output_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_encoder_handle_t encoder = nullptr;
    jpeg_encode_engine_cfg_t engine_config = {
        .timeout_ms = 5000,
    };
    result = jpeg_new_encoder_engine(&engine_config, &encoder);
    if (result != ESP_OK) {
        free(output_buffer);
        return result;
    }

    jpeg_encode_cfg_t encode_config = {
        .height = job.height,
        .width = job.width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
        .pixel_reverse = false,
    };
    uint32_t encoded_size = 0;
    result = jpeg_encoder_process(
        encoder,
        &encode_config,
        job.frame,
        static_cast<uint32_t>(job.frame_size),
        output_buffer,
        static_cast<uint32_t>(output_buffer_size),
        &encoded_size);
    esp_err_t delete_result = jpeg_del_encoder_engine(encoder);
    if (result == ESP_OK && delete_result != ESP_OK) {
        result = delete_result;
    }
    if (result != ESP_OK) {
        free(output_buffer);
        return result;
    }
    if (encoded_size == 0 || encoded_size > output_buffer_size) {
        free(output_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(temporary_path, "wb");
    if (file == nullptr) {
        *system_error = errno;
        free(output_buffer);
        return ESP_FAIL;
    }

    size_t bytes_written = fwrite(output_buffer, 1, encoded_size, file);
    if (bytes_written != encoded_size) {
        *system_error = errno != 0 ? errno : EIO;
        result = ESP_FAIL;
    }
    if (result == ESP_OK && fflush(file) != 0) {
        *system_error = errno;
        result = ESP_FAIL;
    }
    if (fclose(file) != 0 && result == ESP_OK) {
        *system_error = errno;
        result = ESP_FAIL;
    }
    free(output_buffer);

    if (result == ESP_OK && rename(temporary_path, final_path) != 0) {
        *system_error = errno;
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        unlink(temporary_path);
    }
    return result;
}

void photoSaveTask(void *)
{
    PhotoJob job;
    while (true) {
        if (xQueueReceive(save_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        current_state.store(PhotoStorage::State::Saving, std::memory_order_release);
        char filename[sizeof(current_filename)] = {};
        int system_error = 0;
        esp_err_t result = saveJpeg(job, filename, sizeof(filename), &system_error);
        free(job.frame);

        if (result == ESP_OK) {
            memcpy(current_filename, filename, sizeof(current_filename));
            current_error.store(ESP_OK, std::memory_order_relaxed);
            current_system_error.store(0, std::memory_order_relaxed);
            current_state.store(PhotoStorage::State::Saved, std::memory_order_release);
            ESP_LOGI(TAG, "Photo saved: %s/%s", BSP_SD_MOUNT_POINT, filename);
        } else {
            setFailure(result, system_error);
            ESP_LOGE(TAG, "Photo save failed: %s, errno=%d (%s)",
                     esp_err_to_name(result), system_error,
                     system_error != 0 ? strerror(system_error) : "none");
        }
    }
}

} // namespace

esp_err_t PhotoStorage::start()
{
    if (save_queue != nullptr && save_task != nullptr) {
        return ESP_OK;
    }

    save_queue = xQueueCreate(1, sizeof(PhotoJob));
    if (save_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(photoSaveTask, "photo_save", 8192, nullptr, 4, &save_task);
    if (result != pdPASS) {
        vQueueDelete(save_queue);
        save_queue = nullptr;
        save_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool PhotoStorage::requestCapture()
{
    State expected = current_state.load(std::memory_order_acquire);
    while (expected == State::Ready || expected == State::Saved || expected == State::Failed) {
        if (current_state.compare_exchange_weak(expected, State::Preparing, std::memory_order_acq_rel)) {
            current_error.store(ESP_OK, std::memory_order_relaxed);
            current_system_error.store(0, std::memory_order_relaxed);
            current_filename[0] = '\0';
            current_state.store(State::WaitingForFrame, std::memory_order_release);
            return true;
        }
    }
    return false;
}

bool PhotoStorage::captureFrame(const uint8_t *frame, size_t frame_size, uint32_t width, uint32_t height)
{
    State expected = State::WaitingForFrame;
    if (!current_state.compare_exchange_strong(expected, State::Capturing, std::memory_order_acq_rel)) {
        return false;
    }

    if (frame == nullptr || width == 0 || height == 0 ||
        width > SIZE_MAX / height / 2 || frame_size != static_cast<size_t>(width) * height * 2 ||
        frame_size > UINT32_MAX) {
        setFailure(ESP_ERR_INVALID_SIZE, 0);
        return false;
    }

    jpeg_encode_memory_alloc_cfg_t input_memory_config = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    size_t input_buffer_size = 0;
    uint8_t *snapshot = static_cast<uint8_t *>(
        jpeg_alloc_encoder_mem(frame_size, &input_memory_config, &input_buffer_size));
    if (snapshot == nullptr) {
        setFailure(ESP_ERR_NO_MEM, 0);
        return false;
    }
    if (input_buffer_size < frame_size) {
        free(snapshot);
        setFailure(ESP_ERR_INVALID_SIZE, 0);
        return false;
    }

    memcpy(snapshot, frame, frame_size);
    PhotoJob job = {
        .frame = snapshot,
        .frame_size = frame_size,
        .width = width,
        .height = height,
    };
    current_state.store(State::Saving, std::memory_order_release);
    if (xQueueSend(save_queue, &job, 0) != pdTRUE) {
        free(snapshot);
        setFailure(ESP_ERR_INVALID_STATE, 0);
        return false;
    }
    return true;
}

PhotoStorage::State PhotoStorage::state()
{
    return current_state.load(std::memory_order_acquire);
}

esp_err_t PhotoStorage::lastError()
{
    return static_cast<esp_err_t>(current_error.load(std::memory_order_relaxed));
}

int PhotoStorage::lastSystemError()
{
    return current_system_error.load(std::memory_order_relaxed);
}

const char *PhotoStorage::lastFilename()
{
    return current_filename;
}
