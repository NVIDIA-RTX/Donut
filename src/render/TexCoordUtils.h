// Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <donut/engine/SceneTypes.h>
#include <cstring>

namespace donut::render::detail
{
    inline math::float4 GetTexCoordScaleBias(engine::TexCoordFormat format,
        const engine::BufferGroup* buffers, const engine::MeshGeometry* geometry, uint32_t vertex)
    {
        if (format == engine::TexCoordFormat::Unorm16 && buffers)
        {
            const uint32_t hint = geometry ? geometry->texCoordDecodeRangeIndex : ~0u;
            const auto& decode = buffers->getTexCoordDecodeRange(vertex, hint).texCoord1;
            return math::float4(decode.scale, decode.offset);
        }
        return math::float4(1.f, 1.f, 0.f, 0.f);
    }

    // Returns whether the UNORM IA constants need uploading. RenderView invalidates
    // constantsValid on every graphics-state change; direct pass callers always upload.
    inline bool UpdateTexCoordScaleBiasCache(const math::float4& scaleBias, math::float4& lastScaleBias,
        bool cacheEnabled, bool& constantsValid)
    {
        if (!cacheEnabled)
        {
            constantsValid = false;
            return true;
        }
        if (constantsValid && std::memcmp(&lastScaleBias, &scaleBias, sizeof(math::float4)) == 0)
            return false;
        lastScaleBias = scaleBias;
        constantsValid = true;
        return true;
    }
}
