#pragma once

// Переносимый лог: на Android — logcat, на десктопе (сервер) — stdout/stderr.
#ifdef __ANDROID__

#include <android/log.h>
#define LOG_TAG "VBase"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#else

#include <cstdio>
#define LOGI(...) do { std::printf("[I] " __VA_ARGS__); std::printf("\n"); } while (0)
#define LOGW(...) do { std::printf("[W] " __VA_ARGS__); std::printf("\n"); } while (0)
#define LOGE(...) do { std::fprintf(stderr, "[E] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)

#endif
