#pragma once
#include <cstdint>
#if defined(_MSC_VER) && !defined(__clang__)
    extern "C" unsigned char _BitScanReverse(unsigned long* Index, unsigned long Mask);
#pragma intrinsic(_BitScanReverse)
#endif


namespace util { // Custom lightning-fast utility functions
    __forceinline int GetHexCharCount(unsigned long val) noexcept {
        if (val == 0) return 1;
        unsigned long highestBit;
        _BitScanReverse(&highestBit, val);
        return (highestBit / 4) + 1;
    }
    struct { // Convert numbers to hex text
        static constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";
        __forceinline void operator()(wchar_t *const outBuf, unsigned char val) const noexcept {
            outBuf[0] = kHexDigits[(val >> 4) & 0x0F];
            outBuf[1] = kHexDigits[(val) & 0x0F];
            outBuf[2] = L'\0';
        }
        __forceinline void operator()(wchar_t* outBuf, unsigned long val) const noexcept {
            int CharCount = GetHexCharCount(val);
            if (CharCount & 1) CharCount += 1;
            outBuf[CharCount] = L'\0';
            for (int i = CharCount - 1; i >= 0; --i) {
                outBuf[i] = kHexDigits[val & 0x0F];
                val >>= 4;
            }
        }
        __forceinline void operator()(wchar_t* outBuf, unsigned long val, const int charsToFill) const noexcept {
            int CharCount = GetHexCharCount(val);
            const int padding = charsToFill - CharCount;
            if (padding < 0) return;
            CharCount += padding;
            outBuf[CharCount] = L'\0';
            for (int i = CharCount - 1; i >= 0; --i) {
                outBuf[i] = kHexDigits[val & 0x0F];
                val >>= 4;
            }
        }
    } ultowhex;
}
