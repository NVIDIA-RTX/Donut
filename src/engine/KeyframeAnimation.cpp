/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

/*
License for JsonCpp

JsonCpp is Public Domain

The JsonCpp library's source code, including accompanying documentation,
tests and demonstration applications, are licensed under the following
conditions...

Baptiste Lepilleur and The JsonCpp Authors explicitly disclaim copyright in all
jurisdictions which recognize such a disclaimer. In such jurisdictions,
this software is released into the Public Domain.
*/

#include <donut/engine/KeyframeAnimation.h>
#include <donut/core/json.h>
#include <donut/core/log.h>
#include <json/json-forwards.h>
#include <cassert>

using namespace donut::math;
using namespace donut::engine;
using namespace donut::engine::animation;

float4 donut::engine::animation::Interpolate(const InterpolationMode mode,
    const Keyframe& a, const Keyframe& b, const Keyframe& c, const Keyframe& d, const float t, const float dt)
{
    switch (mode)
    {
    case InterpolationMode::Step:
        return b.value;

    case InterpolationMode::Linear:
        return lerp(b.value, c.value, t);

    case InterpolationMode::Slerp: {
        quat qb = quat::fromXYZW(b.value);
        quat qc = quat::fromXYZW(c.value);
        quat qr = slerp(qb, qc, t);
        return float4(qr.x, qr.y, qr.z, qr.w);
    }

    case InterpolationMode::CatmullRomSpline: {
        // https://en.wikipedia.org/wiki/Cubic_Hermite_spline#Interpolation_on_the_unit_interval_with_matched_derivatives_at_endpoints
        // a = p[n-1], b = p[n], c = p[n+1], d = p[n+2]
        float4 i = -a.value + 3.f * b.value - 3.f * c.value + d.value;
        float4 j = 2.f * a.value - 5.f * b.value + 4.f * c.value - d.value;
        float4 k = -a.value + c.value;
        return 0.5f * ((i * t + j) * t + k) * t + b.value;
    }

    case InterpolationMode::HermiteSpline: {
        // https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#appendix-c-spline-interpolation
        const float t2 = t * t;
        const float t3 = t2 * t;
        return (2.f * t3 - 3.f * t2 + 1.f) * b.value
             + (t3 - 2.f * t2 + t) * b.outTangent * dt
             + (-2.f * t3 + 3.f * t2) * c.value
             + (t3 - t2) * c.inTangent * dt;
    }

    default:
        assert(!"Unknown interpolation mode");
        return b.value;
    }
}

std::optional<dm::float4> Sampler::Evaluate(float time, bool extrapolateLastValues) const
{
    const size_t count = m_Keyframes.size();

    if (count == 0)
        return std::optional<float4>();

    if (time <= m_Keyframes[0].time)
        return std::optional(m_Keyframes[0].value);

    if (count == 1 || time >= m_Keyframes[count - 1].time)
    {
        if (extrapolateLastValues)
            return std::optional(m_Keyframes[count - 1].value);
        else
            return std::optional<float4>();
    }
    
    // Use binary search to locate the pair of keyframes (b, c) so that (b.time <= time < c.time).
    // Assume that the keyframe vector is sorted by time.
    // Right limit starts at (count - 2) because we're always looking at consecutive pairs of items, not single items.
    size_t left = 0;
    size_t right = count - 2;
    while (left <= right)
    {
        size_t const middle = (left + right) / 2;

        const float tb = m_Keyframes[middle].time;
        const float tc = m_Keyframes[middle + 1].time;

        if (time < tb)
            right = middle - 1;
        else if (time >= tc)
            left = middle + 1;
        else
        {
            // Found the pair containing the required time, stop.
            left = right = middle;
            break;
        }
    }

    // Load 4 keyframes around the required time.
    // The outside keyframes (a) and (d) are needed for higher-order interpolation.
    size_t const offset = left;
    const Keyframe& b = m_Keyframes[offset];
    const Keyframe& c = m_Keyframes[offset + 1];
    const Keyframe& a = (offset > 0) ? m_Keyframes[offset - 1] : b;
    const Keyframe& d = (offset < count - 2) ? m_Keyframes[offset + 2] : c;
    
    // Validate that the (b, c) keyframes indeed contain the required time.
    if (time < b.time || time >= c.time)
    {
        assert(!"Incorrect keyframe search result! Array not sorted?");
        return std::nullopt;
    }
    
    const float dt = c.time - b.time;
    const float u = (time - b.time) / dt;

    float4 y = Interpolate(m_Mode, a, b, c, d, u, dt);
    
    return std::optional(y);
}

void Sampler::AddKeyframe(const Keyframe keyframe)
{
    m_Keyframes.push_back(keyframe);
}

float Sampler::GetStartTime() const
{
    if (!m_Keyframes.empty())
        return m_Keyframes[0].time;

    return 0.f;
}

float Sampler::GetEndTime() const
{
    if (!m_Keyframes.empty())
        return m_Keyframes[m_Keyframes.size() - 1].time;

    return 0.f;
}

void Sampler::Load(Json::Value& node)
{
    if (node["mode"].isString())
    {
        std::string mode = node["mode"].asString();
        if (mode == "step")
            SetInterpolationMode(InterpolationMode::Step);
        else if (mode == "linear")
            SetInterpolationMode(InterpolationMode::Linear);
        if (mode == "spline")
            SetInterpolationMode(InterpolationMode::CatmullRomSpline);
    }

    bool warningPrinted = false;
    Json::Value& valuesNode = node["values"];
    if (valuesNode.isArray())
    {
        for (Json::Value& valueNode : valuesNode)
        {
            Keyframe keyframe;
            keyframe.time = valueNode["time"].asFloat();
            if (valueNode.isNumeric())
            {
                keyframe.value.x = valueNode.asFloat();
            }
            else if (valueNode.isArray())
            {
                if (valueNode.size() >= 1) keyframe.value.x = valueNode[0].asFloat();  // NOLINT(readability-container-size-empty)
                if (valueNode.size() >= 2) keyframe.value.y = valueNode[1].asFloat();
                if (valueNode.size() >= 3) keyframe.value.z = valueNode[2].asFloat();
                if (valueNode.size() >= 4) keyframe.value.w = valueNode[3].asFloat();
            }
            else if ((valueNode.isObject() || valueNode.isString()) && !warningPrinted)
            {
                log::warning("Objects and strings are not supported as animation keyframe values.");
                warningPrinted = true;
                continue;
            }

            AddKeyframe(keyframe);
        }
        
        std::sort(m_Keyframes.begin(), m_Keyframes.end(),
            [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
}

std::optional<dm::float4> Sequence::Evaluate(const std::string& name, float time, bool extrapolateLastValues)
{
    std::shared_ptr<Sampler> track = GetTrack(name);
    if (!track)
        return std::optional<dm::float4>();

    return track->Evaluate(time, extrapolateLastValues);
}

void Sequence::AddTrack(const std::string& name, const std::shared_ptr<Sampler>& track)
{
    m_Tracks[name] = track;
    m_Duration = std::max(m_Duration, track->GetEndTime());
}

void Sequence::Load(Json::Value& node)
{
    for (auto& trackNode : node)
    {
        auto track = std::make_shared<Sampler>();
        track->Load(trackNode);

        std::string name = trackNode["name"].asString();
        AddTrack(name, track);
    }
}