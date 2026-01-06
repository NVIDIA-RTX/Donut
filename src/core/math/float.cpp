/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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

#include <donut/core/math/float.h>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
    #define USE_F16C 1

    #ifdef _MSC_VER
    #include <intrin.h>
    #else
    #include <cpuid.h>
    #include <immintrin.h>
    #endif
#else
    #define USE_F16C 0
#endif

namespace donut::math
{
    struct FLOAT32
    {
        static constexpr uint32_t EXPONENT_WIDTH = 8;
        static constexpr uint32_t MANTISSA_WIDTH = 23;
        static constexpr bool SUPPORTS_INF = true;
    };

    // Reference: https://en.wikipedia.org/wiki/Half-precision_floating-point_format
    struct FLOAT16
    {
        static constexpr uint32_t EXPONENT_WIDTH = 5;
        static constexpr uint32_t MANTISSA_WIDTH = 10;
        static constexpr bool SUPPORTS_INF = true;
    };

    // Reference: "FP8 Formats for Deep Learning" by Paulius Micikevicius et al.
    // https://arxiv.org/pdf/2209.05433 
    struct FLOAT8E4M3
    {
        static constexpr uint32_t EXPONENT_WIDTH = 4;
        static constexpr uint32_t MANTISSA_WIDTH = 3;
        static constexpr bool SUPPORTS_INF = false;
    };

    // Reference: See above for FLOAT8E4M3
    struct FLOAT8E5M2
    {
        static constexpr uint32_t EXPONENT_WIDTH = 5;
        static constexpr uint32_t MANTISSA_WIDTH = 2;
        static constexpr bool SUPPORTS_INF = true;
    };

    template<typename T>
    struct HELPER : public T
    {
        static constexpr uint32_t EXPONENT_BIAS = (1U << (T::EXPONENT_WIDTH - 1U)) - 1U;
        static constexpr uint32_t EXPONENT_SHIFT = T::MANTISSA_WIDTH;
        static constexpr uint32_t MAX_EXPONENT = (1U << T::EXPONENT_WIDTH) - 1U;
        static constexpr uint32_t SIGN_BIT_SHIFT = T::MANTISSA_WIDTH + T::EXPONENT_WIDTH;
        static constexpr uint32_t SIGN_BIT = 1U << SIGN_BIT_SHIFT;
        static constexpr uint32_t EXPONENT_MASK = MAX_EXPONENT << EXPONENT_SHIFT;
        static constexpr uint32_t MANTISSA_MASK = (1U << T::MANTISSA_WIDTH) - 1U;
        static constexpr uint32_t TOTAL_WIDTH = 1 + T::EXPONENT_WIDTH + T::MANTISSA_WIDTH;
        static constexpr uint32_t EXPONENT_MANTISSA_MASK = SIGN_BIT - 1;
    };

    bool IsF16CSupported()
    {
#if USE_F16C
        constexpr int F16C_BIT = 29;
    #ifdef _MSC_VER
        int cpuInfo[4];
        __cpuid(cpuInfo, 1); // Request CPUID with EAX=1
        bool supported = (cpuInfo[2] >> F16C_BIT) & 1;
    #else
        uint32_t eax, ebx, ecx, edx;
        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
        bool supported = (ecx >> F16C_BIT) & 1;
    #endif
        return supported;
#else // !USE_F16C
        return false;
#endif
    }

    static bool g_SupportsF16C = IsF16CSupported();

    void EnableF16C(bool enable)
    {
        if (enable)
            g_SupportsF16C = IsF16CSupported();
        else
            g_SupportsF16C = false;
    }

    uint32_t asuint(float x)
    {
        return reinterpret_cast<uint32_t&>(x);
    }

    float asfloat(uint32_t x)
    {
        return reinterpret_cast<float&>(x);
    }

    template<typename IN_FORMAT, typename IN_TYPE>
    bool _isinf(IN_TYPE x)
    {
        if constexpr (IN_FORMAT::SUPPORTS_INF)
        {
            return (x & IN_FORMAT::EXPONENT_MANTISSA_MASK) == IN_FORMAT::EXPONENT_MASK;
        }
        else
        {
            return false;
        }
    }

    template<typename IN_FORMAT, typename IN_TYPE>
    bool _isnan(IN_TYPE x)
    {
        if constexpr (IN_FORMAT::SUPPORTS_INF)
        {
            return (x & IN_FORMAT::EXPONENT_MASK) == IN_FORMAT::EXPONENT_MASK
                && (x & IN_FORMAT::MANTISSA_MASK) != 0;
        }
        else
        {
            return (x & IN_FORMAT::EXPONENT_MANTISSA_MASK) == IN_FORMAT::EXPONENT_MANTISSA_MASK;
        }
    }

    template<typename IN_FORMAT, typename IN_TYPE>
    bool _isfinite(IN_TYPE x)
    {
        if constexpr (IN_FORMAT::SUPPORTS_INF)
        {
            return (x & IN_FORMAT::EXPONENT_MASK) != IN_FORMAT::EXPONENT_MASK;
        }
        else
        {
            return (x & IN_FORMAT::EXPONENT_MANTISSA_MASK) != IN_FORMAT::EXPONENT_MANTISSA_MASK;
        }
    }

    template<typename IN_FORMAT, typename IN_TYPE>
    bool _signbit(IN_TYPE x)
    {
        return (x & IN_FORMAT::SIGN_BIT) != 0;
    }

    bool isinf(float16_t x)
    {
        return _isinf<HELPER<FLOAT16>, uint16_t>(x.bits);
    }

    bool isinf(float8e4m3_t x)
    {
        return _isinf<HELPER<FLOAT8E4M3>, uint8_t>(x.bits);
    }

    bool isinf(float8e5m2_t x)
    {
        return _isinf<HELPER<FLOAT8E5M2>, uint8_t>(x.bits);
    }
    
    bool isnan(float16_t x)
    {
        return _isnan<HELPER<FLOAT16>, uint16_t>(x.bits);
    }

    bool isnan(float8e4m3_t x)
    {
        return _isnan<HELPER<FLOAT8E4M3>, uint8_t>(x.bits);
    }

    bool isnan(float8e5m2_t x)
    {
        return _isnan<HELPER<FLOAT8E5M2>, uint8_t>(x.bits);
    }

    bool isfinite(float16_t x)
    {
        return _isfinite<HELPER<FLOAT16>, uint16_t>(x.bits);
    }

    bool isfinite(float8e4m3_t x)
    {
        return _isfinite<HELPER<FLOAT8E4M3>, uint8_t>(x.bits);
    }

    bool isfinite(float8e5m2_t x)
    {
        return _isfinite<HELPER<FLOAT8E5M2>, uint8_t>(x.bits);
    }

    bool signbit(float16_t x)
    {
        return _signbit<HELPER<FLOAT16>, uint16_t>(x.bits);
    }

    bool signbit(float8e4m3_t x)
    {
        return _signbit<HELPER<FLOAT8E4M3>, uint8_t>(x.bits);
    }
    
    bool signbit(float8e5m2_t x)
    {
        return _signbit<HELPER<FLOAT8E5M2>, uint8_t>(x.bits);
    }
    

    template<typename OUT_FORMAT, typename OUT_TYPE>
    OUT_TYPE DownConvert(float const x)
    {
        using IN_FORMAT = HELPER<FLOAT32>;

        static_assert(OUT_FORMAT::MANTISSA_WIDTH < IN_FORMAT::MANTISSA_WIDTH);
        static_assert(OUT_FORMAT::EXPONENT_WIDTH < IN_FORMAT::EXPONENT_WIDTH);
        static_assert(OUT_FORMAT::EXPONENT_BIAS + OUT_FORMAT::MANTISSA_WIDTH < IN_FORMAT::EXPONENT_BIAS);
        static_assert(OUT_FORMAT::TOTAL_WIDTH == sizeof(OUT_TYPE) * 8);
        
        uint32_t const inBits = asuint(x);
        uint32_t const inSign = inBits & IN_FORMAT::SIGN_BIT;
        uint32_t const inExponent = inBits & IN_FORMAT::EXPONENT_MASK;
        uint32_t const inMantissa = inBits & IN_FORMAT::MANTISSA_MASK;

        OUT_TYPE const outSign = OUT_TYPE(inSign >> (IN_FORMAT::SIGN_BIT_SHIFT - OUT_FORMAT::SIGN_BIT_SHIFT));
        OUT_TYPE outExponent = 0;
        OUT_TYPE outMantissa = 0;

        if (inExponent == IN_FORMAT::EXPONENT_MASK) // Inf or NaN
        {
            outExponent = OUT_FORMAT::EXPONENT_MASK;
            if (inMantissa != 0 || !OUT_FORMAT::SUPPORTS_INF) // Input NaN, or Inf is not supported - force NaN
                outMantissa = OUT_FORMAT::MANTISSA_MASK;
        }
        else if (inExponent == 0) // Zero or denormal
        {
            // Input denormals are too small for the output formats, return zero.
            // outExponent and outMantissa are already 0, do nothing.
        }
        else // Normal number that might turn into a denormal or Inf/NaN after down-conversion
        {
            constexpr uint32_t inHiddenOne = (1u << IN_FORMAT::MANTISSA_WIDTH);
            constexpr uint32_t outHiddenOne = (1u << OUT_FORMAT::MANTISSA_WIDTH);

            // Restore the hidden 1 for now
            uint32_t mantissa = inMantissa | inHiddenOne;

            int adjustedExponent = int(inExponent >> IN_FORMAT::EXPONENT_SHIFT)
                - IN_FORMAT::EXPONENT_BIAS + OUT_FORMAT::EXPONENT_BIAS;

            uint32_t remainderBits = IN_FORMAT::MANTISSA_WIDTH - OUT_FORMAT::MANTISSA_WIDTH;
            if (adjustedExponent <= 0) // Convert to denormal
            {
                remainderBits += 1 - adjustedExponent;
                adjustedExponent = 0;
            }

            if (remainderBits > IN_FORMAT::MANTISSA_WIDTH + 1) // Shifting too far to the right
            {
                mantissa = 0;
            }
            else
            {
                uint32_t remainderMask = (1u << remainderBits) - 1u;
                uint32_t remainderHalf = 1u << (remainderBits - 1u);
                uint32_t remainder = mantissa & remainderMask;
                mantissa >>= remainderBits;
                
                // Rounding to nearest, ties to even
                if (remainder > remainderHalf || (remainder == remainderHalf && (mantissa & 1u) != 0))
                {
                    ++mantissa;
                }
            }

            // Mantissa is greater than the output format including the hidden one
            if ((mantissa & (outHiddenOne << 1u)) != 0)
            {
                mantissa >>= 1;
                ++adjustedExponent;
            }
            else if ((mantissa & outHiddenOne) == 0)
            {
                // Hidden one turned into zero, it's a denormal now
                adjustedExponent = 0;
            }
            else if (adjustedExponent == 0)
            {
                // There is a hidden 1, so it cannot be a denormal - set the exponent to 1
                adjustedExponent = 1;
            }

            mantissa &= OUT_FORMAT::MANTISSA_MASK;
            if constexpr (OUT_FORMAT::SUPPORTS_INF)
            {
                // The number turned into Inf, clear the mantissa to make sure it's not a NaN
                if (adjustedExponent >= OUT_FORMAT::MAX_EXPONENT)
                {
                    adjustedExponent = OUT_FORMAT::MAX_EXPONENT;
                    mantissa = 0;
                }
            }
            else
            {
                // Inf not supported - turn any numbers with out-of-range exponents into NaN
                if (adjustedExponent > OUT_FORMAT::MAX_EXPONENT)
                {
                    adjustedExponent = OUT_FORMAT::MAX_EXPONENT;
                    mantissa = OUT_FORMAT::MANTISSA_MASK;
                }
            }

            outExponent = OUT_TYPE(adjustedExponent) << OUT_FORMAT::EXPONENT_SHIFT;
            outMantissa = OUT_TYPE(mantissa);
        }

        return outSign | outExponent | outMantissa;
    }

    template<typename IN_FORMAT, typename IN_TYPE>
    float UpConvert(IN_TYPE const bits)
    {
        using OUT_FORMAT = HELPER<FLOAT32>;

        static_assert(OUT_FORMAT::MANTISSA_WIDTH > IN_FORMAT::MANTISSA_WIDTH);
        static_assert(OUT_FORMAT::EXPONENT_WIDTH > IN_FORMAT::EXPONENT_WIDTH);
        static_assert(OUT_FORMAT::EXPONENT_BIAS > IN_FORMAT::EXPONENT_BIAS + IN_FORMAT::MANTISSA_WIDTH);
        static_assert(IN_FORMAT::TOTAL_WIDTH == sizeof(IN_TYPE) * 8);
        
        IN_TYPE const inSign = bits & IN_FORMAT::SIGN_BIT;
        IN_TYPE const inExponent = bits & IN_FORMAT::EXPONENT_MASK;
        IN_TYPE const inMantissa = bits & IN_FORMAT::MANTISSA_MASK;

        uint32_t const outSign = uint32_t(inSign) << (OUT_FORMAT::SIGN_BIT_SHIFT - IN_FORMAT::SIGN_BIT_SHIFT);
        uint32_t outExponent = uint32_t(inExponent) << (OUT_FORMAT::EXPONENT_SHIFT - IN_FORMAT::EXPONENT_SHIFT);
        outExponent += (OUT_FORMAT::EXPONENT_BIAS - IN_FORMAT::EXPONENT_BIAS) << OUT_FORMAT::EXPONENT_SHIFT;
        uint32_t outMantissa = uint32_t(inMantissa) << (OUT_FORMAT::MANTISSA_WIDTH - IN_FORMAT::MANTISSA_WIDTH);

        if (inExponent == IN_FORMAT::EXPONENT_MASK && (IN_FORMAT::SUPPORTS_INF || inMantissa == IN_FORMAT::MANTISSA_MASK)) // Inf or NaN
        {
            outExponent = OUT_FORMAT::EXPONENT_MASK;
        }
        else if (inExponent == 0) // Denormal
        {
            if (inMantissa != 0) // Non-zero denormal - convert to a normal number, there must be enough exponent bits for that
            {
#ifdef _MSC_VER
                uint32_t leadingZeros = __lzcnt(uint32_t(inMantissa));
#else
                uint32_t leadingZeros = __builtin_clz(uint32_t(inMantissa));
#endif
                // Don't count the bits from uint32_t higher than the mantissa
                leadingZeros -= 32 - IN_FORMAT::MANTISSA_WIDTH;
                
                // Shift the mantissa so that its highest "one" bit becomes the hidden "one"
                outMantissa = (outMantissa << (leadingZeros + 1)) & OUT_FORMAT::MANTISSA_MASK;
                // Correct the exponent after that mantissa shift.
                // This assumes that the result is going to be positive, and the static_assert on EXPONENT_BIAS above
                // makes sure that's always true.
                outExponent -= leadingZeros << OUT_FORMAT::EXPONENT_SHIFT;
            }
            else // Zero
            {
                outExponent = 0;
            }
        }

        return asfloat(outSign | outExponent | outMantissa);
    }

    float16_t Float32ToFloat16(float x)
    {
#if USE_F16C
        if (g_SupportsF16C)
        {
            __m128 a = _mm_setr_ps(x, 0.f, 0.f, 0.f);
            __m128i b = _mm_cvtps_ph(a, _MM_FROUND_TO_NEAREST_INT);
            return float16_t { uint16_t(_mm_cvtsi128_si32(b)) };
        }
#endif

        uint16_t const a = DownConvert<HELPER<FLOAT16>, uint16_t>(x);
        return float16_t{ a };
    }

    float16_t2 Float32ToFloat16x2(float2 x)
    {
#if USE_F16C
        if (g_SupportsF16C)
        {
            __m128 a = _mm_setr_ps(x.x, x.y, 0.f, 0.f);
            __m128i b = _mm_cvtps_ph(a, _MM_FROUND_TO_NEAREST_INT);
            return float16_t2 { uint32_t(_mm_cvtsi128_si32(b)) };
        }
#endif

        uint16_t const a = DownConvert<HELPER<FLOAT16>, uint16_t>(x.x);
        uint16_t const b = DownConvert<HELPER<FLOAT16>, uint16_t>(x.y);
        uint32_t packed = uint32_t(a) | (uint32_t(b) << 16);
        return float16_t2{ packed };
    }

    float16_t4 Float32ToFloat16x4(float4 x)
    {
#if USE_F16C
        if (g_SupportsF16C)
        {
            __m128 a = _mm_setr_ps(x.x, x.y, x.z, x.w);
            __m128i b = _mm_cvtps_ph(a, _MM_FROUND_TO_NEAREST_INT);
            return float16_t4 { uint64_t(_mm_cvtsi128_si64(b)) };
        }
#endif
        uint16_t const a = DownConvert<HELPER<FLOAT16>, uint16_t>(x.x);
        uint16_t const b = DownConvert<HELPER<FLOAT16>, uint16_t>(x.y);
        uint16_t const c = DownConvert<HELPER<FLOAT16>, uint16_t>(x.z);
        uint16_t const d = DownConvert<HELPER<FLOAT16>, uint16_t>(x.w);
        uint64_t packed = uint64_t(a) | (uint64_t(b) << 16) | (uint64_t(c) << 32) | (uint64_t(d) << 48);
        return float16_t4{ packed };
    }

    float Float16ToFloat32(float16_t x)
    {
#if USE_F16C
        if (g_SupportsF16C)
        {
            __m128i a = _mm_cvtsi32_si128(x.bits);
            __m128 b = _mm_cvtph_ps(a);
            float f;
            _mm_store_ss(&f, b);
            return f;
        }
#endif

        return UpConvert<HELPER<FLOAT16>, uint16_t>(x.bits);
    }

    float2 Float16ToFloat32x2(float16_t2 x)
    {
#if USE_F16C
        if (g_SupportsF16C)
        {
            __m128i a = _mm_cvtsi32_si128(x.bits);
            __m128 b = _mm_cvtph_ps(a);
            float f[4];
            _mm_store_ps(f, b);
            return float2(f);
        }
#endif

        float2 v;
        v.x = UpConvert<HELPER<FLOAT16>, uint16_t>(uint16_t(x.bits & 0xffff));
        v.y = UpConvert<HELPER<FLOAT16>, uint16_t>(uint16_t(x.bits >> 16));
        return v;
    }

    float4 Float16ToFloat32x4(float16_t4 x)
    {
#if USE_F16C
        if (g_SupportsF16C)
        {
            __m128i a = _mm_cvtsi64_si128(x.bits);
            __m128 b = _mm_cvtph_ps(a);
            float f[4];
            _mm_store_ps(f, b);
            return float4(f);
        }
#endif

        float4 v;
        v.x = UpConvert<HELPER<FLOAT16>, uint16_t>(uint16_t(x.bits & 0xffff));
        v.y = UpConvert<HELPER<FLOAT16>, uint16_t>(uint16_t((x.bits >> 16) & 0xffff));
        v.z = UpConvert<HELPER<FLOAT16>, uint16_t>(uint16_t((x.bits >> 32) & 0xffff));
        v.w = UpConvert<HELPER<FLOAT16>, uint16_t>(uint16_t((x.bits >> 48) & 0xffff));
        return v;
    }

    float8e4m3_t Float32ToFloat8E4M3(float x)
    {
        uint8_t const a = DownConvert<HELPER<FLOAT8E4M3>, uint8_t>(x);
        return float8e4m3_t{ a };
    }

    float8e4m3_t4 Float32ToFloat8E4M3x4(float4 x)
    {
        uint8_t const a = DownConvert<HELPER<FLOAT8E4M3>, uint8_t>(x.x);
        uint8_t const b = DownConvert<HELPER<FLOAT8E4M3>, uint8_t>(x.y);
        uint8_t const c = DownConvert<HELPER<FLOAT8E4M3>, uint8_t>(x.z);
        uint8_t const d = DownConvert<HELPER<FLOAT8E4M3>, uint8_t>(x.w);
        uint32_t packed = uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
        return float8e4m3_t4{ packed };
    }

    float Float8E4M3ToFloat32(float8e4m3_t x)
    {
        return UpConvert<HELPER<FLOAT8E4M3>, uint8_t>(x.bits);
    }

    float4 Float8E4M3ToFloat32x4(float8e4m3_t4 x)
    {
        float4 v;
        v.x = UpConvert<HELPER<FLOAT8E4M3>, uint8_t>(uint8_t(x.bits & 0xff));
        v.y = UpConvert<HELPER<FLOAT8E4M3>, uint8_t>(uint8_t((x.bits >> 8) & 0xff));
        v.z = UpConvert<HELPER<FLOAT8E4M3>, uint8_t>(uint8_t((x.bits >> 16) & 0xff));
        v.w = UpConvert<HELPER<FLOAT8E4M3>, uint8_t>(uint8_t((x.bits >> 24) & 0xff));
        return v;
    }

    float8e5m2_t Float32ToFloat8E5M2(float x)
    {
        uint8_t const a = DownConvert<HELPER<FLOAT8E5M2>, uint8_t>(x);
        return float8e5m2_t{ a };
    }

    float8e5m2_t4 Float32ToFloat8E5M2x4(float4 x)
    {
        uint8_t const a = DownConvert<HELPER<FLOAT8E5M2>, uint8_t>(x.x);
        uint8_t const b = DownConvert<HELPER<FLOAT8E5M2>, uint8_t>(x.y);
        uint8_t const c = DownConvert<HELPER<FLOAT8E5M2>, uint8_t>(x.z);
        uint8_t const d = DownConvert<HELPER<FLOAT8E5M2>, uint8_t>(x.w);
        uint32_t packed = uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
        return float8e5m2_t4{ packed };
    }

    float Float8E5M2ToFloat32(float8e5m2_t x)
    {
        return UpConvert<HELPER<FLOAT8E5M2>, uint8_t>(x.bits);
    }

    float4 Float8E5M2ToFloat32x4(float8e5m2_t4 x)
    {
        float4 v;
        v.x = UpConvert<HELPER<FLOAT8E5M2>, uint8_t>(uint8_t(x.bits & 0xff));
        v.y = UpConvert<HELPER<FLOAT8E5M2>, uint8_t>(uint8_t((x.bits >> 8) & 0xff));
        v.z = UpConvert<HELPER<FLOAT8E5M2>, uint8_t>(uint8_t((x.bits >> 16) & 0xff));
        v.w = UpConvert<HELPER<FLOAT8E5M2>, uint8_t>(uint8_t((x.bits >> 24) & 0xff));
        return v;
    }
}