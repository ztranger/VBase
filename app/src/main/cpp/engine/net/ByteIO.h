#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Явная сериализация протокола (P2-15): примитивы пишутся/читаются побайтно в little-endian,
// БЕЗ зависимости от раскладки/паддинга/endianness компилятора (в отличие от memcpy POD-структур).
// Чтение — с проверкой границ: попытка прочитать за концом буфера ставит overflow, и вызывающий
// отвергает пакет по !ok(). Платформонезависимо; float — IEEE-754 (все целевые платформы).
class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)(v & 0xFF)); u8((uint8_t)((v >> 8) & 0xFF)); }
    void u32(uint32_t v) { u16((uint16_t)(v & 0xFFFF)); u16((uint16_t)((v >> 16) & 0xFFFF)); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const void* data, size_t len)
        : p_(static_cast<const uint8_t*>(data)), end_(static_cast<const uint8_t*>(data) + len) {}
    bool ok() const { return !overflow_; }  // false = пакет был короче, чем прочитано
    uint8_t u8() {
        if (p_ + 1 > end_) { overflow_ = true; return 0; }
        return *p_++;
    }
    uint16_t u16() { uint16_t a = u8(); uint16_t b = u8(); return (uint16_t)(a | (b << 8)); }
    uint32_t u32() { uint32_t a = u16(); uint32_t b = u16(); return a | (b << 16); }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool overflow_ = false;
};
