/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <stdint.h>
#include <limits>
#include "vector.h"

namespace donut::math
{
    // 16-bit floating point number, E5M10.
    struct float16_t
    {
        uint16_t bits;
    };

    // Vector of 2 FP16 numbers packed into a single uint32.
    struct float16_t2
    {
        uint32_t bits;
    };

    // Vector of 4 FP16 numbers packed into a single uint64.
    struct float16_t4
    {
        uint64_t bits;
    };

    // 8-bit floating point number, E4M3.
    // This is FLOAT8E4M3FN, not FLOAT8E4M3FNUZ.
    // It does not have an infinity, has two NaNs (0x7f and 0xff), and a signed zero.
    struct float8e4m3_t
    {
        uint8_t bits;
    };

    // Vector of 4 E4M3 numbers packed into a single uint32.
    struct float8e4m3_t4
    {
        uint32_t bits;
    };

    // 8-bit floating point number, E5M2.
    // This is FLOAT8E5M2, not FLOAT8E5M2FNUZ.
    // It has a signed infinity (0x7c and 0xfc), 6 NaNs (0x7d..0x7f, 0xfd..0xff), and a signed zero.
    struct float8e5m2_t
    {
        uint8_t bits;
    };

    // Vector of 4 E5M2 numbers packed into a single uint32.
    struct float8e5m2_t4
    {
        uint32_t bits;
    };

#define DM_DECLARE_EQUALITY_OPERATORS(TY)\
    inline bool operator==(TY a, TY b) { return a.bits == b.bits; } \
    inline bool operator!=(TY a, TY b) { return a.bits != b.bits; }
    
    DM_DECLARE_EQUALITY_OPERATORS(float16_t)
    DM_DECLARE_EQUALITY_OPERATORS(float16_t2)
    DM_DECLARE_EQUALITY_OPERATORS(float16_t4)
    DM_DECLARE_EQUALITY_OPERATORS(float8e4m3_t)
    DM_DECLARE_EQUALITY_OPERATORS(float8e4m3_t4)
    DM_DECLARE_EQUALITY_OPERATORS(float8e5m2_t)
    DM_DECLARE_EQUALITY_OPERATORS(float8e5m2_t4)

#undef DM_DECLARE_EQUALITY_OPERATORS

    uint32_t asuint(float x);
    float asfloat(uint32_t x);

    bool isinf(float16_t x);
    bool isinf(float8e4m3_t x); // Always returns false, provided for completeness.
    bool isinf(float8e5m2_t x);
    
    bool isnan(float16_t x);
    bool isnan(float8e4m3_t x);
    bool isnan(float8e5m2_t x);

    bool isfinite(float16_t x);
    bool isfinite(float8e4m3_t x);
    bool isfinite(float8e5m2_t x);

    bool signbit(float16_t x);
    bool signbit(float8e4m3_t x);
    bool signbit(float8e5m2_t x);

    // Returns true if the CPU supports F16C instructions (x64 only)
    // See https://en.wikipedia.org/wiki/F16C
    bool IsF16CSupported();
    
    // Enables the use of F16C instructions, if supported (x64 only)
    void EnableF16C(bool enable);

    float16_t Float32ToFloat16(float x);
    float16_t2 Float32ToFloat16x2(float2 x);
    float16_t4 Float32ToFloat16x4(float4 x);
    float Float16ToFloat32(float16_t x);
    float2 Float16ToFloat32x2(float16_t2 x);
    float4 Float16ToFloat32x4(float16_t4 x);

    float8e4m3_t Float32ToFloat8E4M3(float x);
    float8e4m3_t4 Float32ToFloat8E4M3x4(float4 x);
    float Float8E4M3ToFloat32(float8e4m3_t x);
    float4 Float8E4M3ToFloat32x4(float8e4m3_t4 x);
    
    float8e5m2_t Float32ToFloat8E5M2(float x);
    float8e5m2_t4 Float32ToFloat8E5M2x4(float4 x);
    float Float8E5M2ToFloat32(float8e5m2_t x);
    float4 Float8E5M2ToFloat32x4(float8e5m2_t4 x);
}

namespace std
{
    template<> class numeric_limits<donut::math::float16_t>
    {
    public:
        static constexpr donut::math::float16_t(min)() noexcept {
            return donut::math::float16_t{ 0x0400 }; // 6.1035e-5
        }
        static constexpr donut::math::float16_t(max)() noexcept {
            return donut::math::float16_t{ 0x7bff }; // 65504.0
        }
        static constexpr donut::math::float16_t lowest() noexcept {
            return min();
        }
        static constexpr donut::math::float16_t epsilon() noexcept {
            return donut::math::float16_t{ 0x1400 }; // f16(0x3c01) - f16(0x3c00)
        }
        static constexpr donut::math::float16_t round_error() noexcept {
            return donut::math::float16_t{ 0x3800 }; // 0.5
        }
        static constexpr donut::math::float16_t denorm_min() noexcept {
            return donut::math::float16_t{ 0x0001 }; // 5.9604e-8
        }
        static constexpr donut::math::float16_t infinity() noexcept {
            return donut::math::float16_t{ 0x7C00 };
        }
        static constexpr donut::math::float16_t quiet_NaN() noexcept {
            return donut::math::float16_t{ 0x7FFF };
        }

        static constexpr int digits             = 11;
        static constexpr int max_exponent       = 16;
        static constexpr int min_exponent       = -14;
        static constexpr int radix              = 2;
        static constexpr bool has_infinity      = true;
        static constexpr bool has_quiet_NaN     = true;
        static constexpr bool has_signaling_NaN = false;
        static constexpr bool is_bounded        = true;
        static constexpr bool is_exact          = true;
        static constexpr bool is_iec559         = false;
        static constexpr bool is_integer        = true;
        static constexpr bool is_signed         = true;
        static constexpr bool is_specialized    = true;
        static constexpr float_round_style round_style = round_to_nearest;
    };

    template<> class numeric_limits<donut::math::float8e4m3_t>
    {
    public:
        static constexpr donut::math::float8e4m3_t(min)() noexcept {
            return donut::math::float8e4m3_t{ 0x08 }; // 0.01562
        }
        static constexpr donut::math::float8e4m3_t(max)() noexcept {
            return donut::math::float8e4m3_t{ 0x7e }; // 448.0
        }
        static constexpr donut::math::float8e4m3_t lowest() noexcept {
            return min();
        }
        static constexpr donut::math::float8e4m3_t epsilon() noexcept {
            return donut::math::float8e4m3_t{ 0x20 }; // f8(0x39) - f8(0x38)
        }
        static constexpr donut::math::float8e4m3_t round_error() noexcept {
            return donut::math::float8e4m3_t{ 0x30 }; // 0.5
        }
        static constexpr donut::math::float8e4m3_t denorm_min() noexcept {
            return donut::math::float8e4m3_t{ 0x01 }; // 0.001953	
        }
        static constexpr donut::math::float8e4m3_t quiet_NaN() noexcept {
            return donut::math::float8e4m3_t{ 0x7f };
        }

        static constexpr int digits             = 4;
        static constexpr int max_exponent       = 8;
        static constexpr int min_exponent       = -6;
        static constexpr int radix              = 2;
        static constexpr bool has_infinity      = false;
        static constexpr bool has_quiet_NaN     = true;
        static constexpr bool has_signaling_NaN = false;
        static constexpr bool is_bounded        = true;
        static constexpr bool is_exact          = true;
        static constexpr bool is_iec559         = false;
        static constexpr bool is_integer        = true;
        static constexpr bool is_signed         = true;
        static constexpr bool is_specialized    = true;
        static constexpr float_round_style round_style = round_to_nearest;
    };
    
    template<> class numeric_limits<donut::math::float8e5m2_t>
    {
    public:
        static constexpr donut::math::float8e5m2_t(min)() noexcept {
            return donut::math::float8e5m2_t{ 0x04 }; // 0.000061
        }
        static constexpr donut::math::float8e5m2_t(max)() noexcept {
            return donut::math::float8e5m2_t{ 0x7b }; // 57344.0
        }
        static constexpr donut::math::float8e5m2_t lowest() noexcept {
            return min();
        }
        static constexpr donut::math::float8e5m2_t epsilon() noexcept {
            return donut::math::float8e5m2_t{ 0x34 }; // f8(0x3d) - f8(0x3c)
        }
        static constexpr donut::math::float8e5m2_t round_error() noexcept {
            return donut::math::float8e5m2_t{ 0x38 }; // 0.5
        }
        static constexpr donut::math::float8e5m2_t denorm_min() noexcept {
            return donut::math::float8e5m2_t{ 0x01 }; // 0.0000153
        }
        static constexpr donut::math::float8e5m2_t infinity() noexcept {
            return donut::math::float8e5m2_t{ 0x7c };
        }
        static constexpr donut::math::float8e5m2_t quiet_NaN() noexcept {
            return donut::math::float8e5m2_t{ 0x7f };
        }

        static constexpr int digits             = 3;
        static constexpr int max_exponent       = 16;
        static constexpr int min_exponent       = -14;
        static constexpr int radix              = 2;
        static constexpr bool has_infinity      = true;
        static constexpr bool has_quiet_NaN     = true;
        static constexpr bool has_signaling_NaN = false;
        static constexpr bool is_bounded        = true;
        static constexpr bool is_exact          = true;
        static constexpr bool is_iec559         = false;
        static constexpr bool is_integer        = true;
        static constexpr bool is_signed         = true;
        static constexpr bool is_specialized    = true;
        static constexpr float_round_style round_style = round_to_nearest;
    };
}