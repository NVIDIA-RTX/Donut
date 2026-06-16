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

#include <cassert>
#include <donut/app/Camera.h>
#include <donut/engine/SceneGraph.h>
#include <donut/engine/View.h>

using namespace donut::math;
using namespace donut::app;

void BaseCamera::UpdateWorldToView()
{
    m_MatTranslatedWorldToView = affine3::from_cols(m_CameraRight, m_CameraUp, m_CameraDir, 0.f);
    m_MatWorldToView = translation(-m_CameraPos) * m_MatTranslatedWorldToView;
}

void BaseCamera::BaseLookAt(float3 cameraPos, float3 cameraTarget, float3 cameraUp)
{
    this->m_CameraPos = cameraPos;
    this->m_CameraDir = normalize(cameraTarget - cameraPos);
    this->m_CameraUp = normalize(cameraUp);
    this->m_CameraRight = normalize(cross(this->m_CameraUp, this->m_CameraDir));
    this->m_CameraUp = normalize(cross(this->m_CameraDir, this->m_CameraRight));

    UpdateWorldToView();
}

void FirstPersonCamera::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (m_KeyboardMap.find(key) == m_KeyboardMap.end())
    {
        return;
    }

    auto cameraKey = m_KeyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        m_KeyboardState[cameraKey] = true;
    }
    else {
        m_KeyboardState[cameraKey] = false;
    }
}

void FirstPersonCamera::MousePosUpdate(double xpos, double ypos)
{
    m_MousePos = { float(xpos), float(ypos) };
}

void FirstPersonCamera::MouseButtonUpdate(int button, int action, int mods)
{
    if (m_MouseButtonMap.find(button) == m_MouseButtonMap.end())
    {
        return;
    }

    auto cameraButton = m_MouseButtonMap.at(button);
    if (action == GLFW_PRESS)
    {
        m_MouseButtonState[cameraButton] = true;
    }
    else {
        m_MouseButtonState[cameraButton] = false;
    }
}

void FirstPersonCamera::LookAt(float3 cameraPos, float3 cameraTarget, float3 cameraUp)
{
    // Make the base method public.
    BaseLookAt(cameraPos, cameraTarget, cameraUp);
    m_MouseMotionAccumulator = 0.f;
    m_CameraMoveDamp = 0.f;
    m_CameraMovePrev = 0.f;
}

void FirstPersonCamera::LookTo(dm::float3 cameraPos, dm::float3 cameraDir, dm::float3 cameraUp)
{
    BaseLookAt(cameraPos, cameraPos + cameraDir, cameraUp);
    m_MouseMotionAccumulator = 0.f;
    m_CameraMoveDamp = 0.f;
    m_CameraMovePrev = 0.f;
}

std::pair<bool, float3> FirstPersonCamera::AnimateTranslation(float deltaT)
{
    bool cameraDirty = false;
    float moveStep = deltaT * m_MoveSpeed;
    float3 cameraMoveVec = 0.f;

    if (m_KeyboardState[KeyboardControls::SpeedUp])
        moveStep *= 3.f;

    if (m_KeyboardState[KeyboardControls::SlowDown])
        moveStep *= .1f;

    if (m_KeyboardState[KeyboardControls::MoveForward])
    {
        cameraDirty = true;
        cameraMoveVec += m_CameraDir * moveStep;
    }

    if (m_KeyboardState[KeyboardControls::MoveBackward])
    {
        cameraDirty = true;
        cameraMoveVec += -m_CameraDir * moveStep;
    }

    if (m_KeyboardState[KeyboardControls::MoveLeft])
    {
        cameraDirty = true;
        cameraMoveVec += -m_CameraRight * moveStep;
    }

    if (m_KeyboardState[KeyboardControls::MoveRight])
    {
        cameraDirty = true;
        cameraMoveVec += m_CameraRight * moveStep;
    }

    if (m_KeyboardState[KeyboardControls::MoveUp])
    {
        cameraDirty = true;
        cameraMoveVec += m_CameraUp * moveStep;
    }

    if (m_KeyboardState[KeyboardControls::MoveDown])
    {
        cameraDirty = true;
        cameraMoveVec += -m_CameraUp * moveStep;
    }
    return std::make_pair(cameraDirty, cameraMoveVec);
}

void FirstPersonCamera::UpdateCamera(dm::float3 cameraMoveVec, dm::affine3 cameraRotation)
{
    m_CameraPos += cameraMoveVec;
    m_CameraDir = normalize(cameraRotation.transformVector(m_CameraDir));
    m_CameraUp = normalize(cameraRotation.transformVector(m_CameraUp));
    m_CameraRight = normalize(cross(m_CameraUp, m_CameraDir));

    UpdateWorldToView();
}

std::pair<bool, affine3> FirstPersonCamera::AnimateRoll(affine3 initialRotation)
{
    bool cameraDirty = false;
    affine3 cameraRotation = initialRotation;
    if (m_KeyboardState[KeyboardControls::RollLeft] ||
        m_KeyboardState[KeyboardControls::RollRight])
    {
        float roll = float(m_KeyboardState[KeyboardControls::RollLeft]) * -m_RotateSpeed * 2.0f +
            float(m_KeyboardState[KeyboardControls::RollRight]) * m_RotateSpeed * 2.0f;

        cameraRotation = rotation(m_CameraDir, roll) * cameraRotation;
        cameraDirty = true;
    }
    return std::make_pair(cameraDirty, cameraRotation);
}

void FirstPersonCamera::Animate(float deltaT)
{
    // Track mouse delta.
    // Use m_IsDragging to avoid random camera rotations when clicking inside an inactive window.
    float2 mouseMove = 0.f;
    if (m_MouseButtonState[MouseButtons::Left])
    {
        if (m_IsDragging)
            mouseMove = m_MousePos - m_MousePosPrev;

        m_IsDragging = true;
    }
    else
    {
        m_IsDragging = false;
    }
    m_MousePosPrev = m_MousePos;

    bool cameraDirty = false;
    affine3 cameraRotation = affine3::identity();

    // handle mouse rotation first
    // this will affect the movement vectors in the world matrix, which we use below
    if (m_MouseButtonState[MouseButtons::Left] && (mouseMove.x != 0 || mouseMove.y != 0))
    {
        float yaw = m_RotateSpeed * mouseMove.x;
        float pitch = m_RotateSpeed * mouseMove.y;

        cameraRotation = rotation(float3(0.f, 1.f, 0.f), yaw);
        cameraRotation = rotation(m_CameraRight, pitch) * cameraRotation;

        cameraDirty = true;
    }

    // handle keyboard roll next
    auto rollResult = AnimateRoll(cameraRotation);
    cameraDirty |= rollResult.first;
    cameraRotation = rollResult.second;

    // handle translation
    auto translateResult = AnimateTranslation(deltaT);
    cameraDirty |= translateResult.first;
    const float3& cameraMoveVec = translateResult.second;

    if (cameraDirty)
    {
        UpdateCamera(cameraMoveVec, cameraRotation);
    }
}

void FirstPersonCamera::AnimateSmooth(float deltaT)
{
    const float c_DampeningRate = 7.5f;
    float dampenWeight = exp(-c_DampeningRate * deltaT);

    // Track mouse delta.
    // Use m_IsDragging to avoid random camera rotations when clicking inside an inactive window.
    if (m_MouseButtonState[MouseButtons::Left])
    {
        if (m_IsDragging)
        {
            // Use an accumulator to keep the camera animating after mouse button has been released.
            m_MouseMotionAccumulator += m_MousePos - m_MousePosPrev;
        }

        m_IsDragging = true;
    }
    else
    {
        m_IsDragging = false;
    }
    m_MousePosPrev = m_MousePos;

    float2 mouseMove = m_MouseMotionAccumulator * (1.f - dampenWeight);
    m_MouseMotionAccumulator *= dampenWeight;

    affine3 cameraRotation = affine3::identity();

    // handle mouse rotation first
    // this will affect the movement vectors in the world matrix, which we use below
    if (mouseMove.x || mouseMove.y)
    {
        float yaw = m_RotateSpeed * mouseMove.x;
        float pitch = m_RotateSpeed * mouseMove.y;

        cameraRotation = rotation(float3(0.f, 1.f, 0.f), -yaw);
        cameraRotation = rotation(m_CameraRight, -pitch) * cameraRotation;
    }

    // handle keyboard roll next
    auto rollResult = AnimateRoll(cameraRotation);
    cameraRotation = rollResult.second;

    // handle translation
    auto translateResult = AnimateTranslation(deltaT);
    const float3& cameraMoveVec = translateResult.second;

    m_CameraMoveDamp = lerp(cameraMoveVec, m_CameraMovePrev, dampenWeight);
    m_CameraMovePrev = m_CameraMoveDamp;

    UpdateCamera(m_CameraMoveDamp, cameraRotation);
}

void ThirdPersonCamera::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (m_KeyboardMap.find(key) == m_KeyboardMap.end())
    {
        return;
    }

    auto cameraKey = m_KeyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        m_KeyboardState[cameraKey] = true;
    }
    else {
        m_KeyboardState[cameraKey] = false;
    }
}

void ThirdPersonCamera::MousePosUpdate(double xpos, double ypos)
{
    m_MousePos = float2(float(xpos), float(ypos));
}

void ThirdPersonCamera::MouseButtonUpdate(int button, int action, int mods)
{
    const bool pressed = (action == GLFW_PRESS);

    switch(m_MouseState)
    {
    case MouseState::Idle:
        if (pressed)
        {
            if (button == GLFW_MOUSE_BUTTON_LEFT)
                m_MouseState = MouseState::Orbiting;
            else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
                m_MouseState = MouseState::Panning;
            m_MousePosPrev = m_MousePos;
        }
        break;

    case MouseState::Orbiting:
        if (!pressed && button == GLFW_MOUSE_BUTTON_LEFT)
            m_MouseState = MouseState::Idle;
        break;

    case MouseState::Panning:
        if (!pressed && button == GLFW_MOUSE_BUTTON_MIDDLE)
            m_MouseState = MouseState::Idle;
        break;
    }
}

void ThirdPersonCamera::MouseScrollUpdate(double xoffset, double yoffset)
{
    const float scrollFactor = 1.15f;
    m_Distance = clamp(m_Distance * (yoffset < 0 ? scrollFactor : 1.0f / scrollFactor), m_MinDistance,  m_MaxDistance);
}

void ThirdPersonCamera::JoystickUpdate(int axis, float value)
{
    switch (axis)
    {
    case GLFW_GAMEPAD_AXIS_RIGHT_X: m_DeltaYaw = value; break;
    case GLFW_GAMEPAD_AXIS_RIGHT_Y: m_DeltaPitch = value; break;
    default: break;
    }
}

void ThirdPersonCamera::JoystickButtonUpdate(int button, bool pressed)
{
    switch (button)
    {
    case GLFW_GAMEPAD_BUTTON_B: if (pressed) m_DeltaDistance -= 1; break;
    case GLFW_GAMEPAD_BUTTON_A: if (pressed) m_DeltaDistance += 1; break;
    default: break;
    }
}

void ThirdPersonCamera::SetRotation(float yaw, float pitch)
{
    m_Yaw = yaw;
    m_Pitch = pitch;
}

void ThirdPersonCamera::SetView(const engine::PlanarView& view)
{
    m_ProjectionMatrix = view.GetProjectionMatrix(false);
    m_InverseProjectionMatrix = view.GetInverseProjectionMatrix(false);
    auto viewport = view.GetViewport();
    m_ViewportSize = float2(viewport.width(), viewport.height());
}

void ThirdPersonCamera::AnimateOrbit(float deltaT, float2 mouseMove)
{
    m_Yaw -= m_RotateSpeed * mouseMove.x;
    m_Pitch += m_RotateSpeed * mouseMove.y;

    const float ORBIT_SENSITIVITY = 1.5f;
    const float ZOOM_SENSITIVITY = 40.f;
    m_Distance += ZOOM_SENSITIVITY * deltaT * m_DeltaDistance;
    m_Yaw += ORBIT_SENSITIVITY * deltaT * m_DeltaYaw;
    m_Pitch += ORBIT_SENSITIVITY * deltaT * m_DeltaPitch;

    m_Distance = clamp(m_Distance, m_MinDistance, m_MaxDistance);
    
    m_Pitch = clamp(m_Pitch, PI_f * -0.5f, PI_f * 0.5f);
    
    m_DeltaDistance = 0;
    m_DeltaYaw = 0;
    m_DeltaPitch = 0;
}

void ThirdPersonCamera::AnimateTranslation(const dm::float3x3& viewMatrix)
{
    // If the view parameters have never been set, we can't translate
    if (m_ViewportSize.x <= 0.f || m_ViewportSize.y <= 0.f)
        return;

    if (all(m_MousePos == m_MousePosPrev))
        return;

    float4 oldClipPos = float4(0.f, 0.f, m_Distance, 1.f) * m_ProjectionMatrix;
    oldClipPos /= oldClipPos.w;
    oldClipPos.x = 2.f * (m_MousePosPrev.x) / m_ViewportSize.x - 1.f;
    oldClipPos.y = 1.f - 2.f * (m_MousePosPrev.y) / m_ViewportSize.y;
    float4 newClipPos = oldClipPos;
    newClipPos.x = 2.f * (m_MousePos.x) / m_ViewportSize.x - 1.f;
    newClipPos.y = 1.f - 2.f * (m_MousePos.y) / m_ViewportSize.y;

    float4 oldViewPos = oldClipPos * m_InverseProjectionMatrix;
    oldViewPos /= oldViewPos.w;
    float4 newViewPos = newClipPos * m_InverseProjectionMatrix;
    newViewPos /= newViewPos.w;

    float2 viewMotion = oldViewPos.xy() - newViewPos.xy();

    m_TargetPos -= viewMotion.x * viewMatrix.row0;

    if (m_KeyboardState[KeyboardControls::HorizontalPan])
    {
        float3 horizontalForward = float3(viewMatrix.row2.x, 0.f, viewMatrix.row2.z);
        float horizontalLength = length(horizontalForward);
        if (horizontalLength == 0.f)
            horizontalForward = float3(viewMatrix.row1.x, 0.f, viewMatrix.row1.z);
        horizontalForward = normalize(horizontalForward);
        m_TargetPos += viewMotion.y * horizontalForward * 1.5f;
    }
    else
        m_TargetPos += viewMotion.y * viewMatrix.row1;
}

void ThirdPersonCamera::Animate(float deltaT)
{
    quat orbit = rotationQuat(float3(m_Pitch, m_Yaw, 0));
    const auto targetRotation = orbit.toMatrix();

    switch(m_MouseState)
    {
    case MouseState::Orbiting:
        {
            float2 mouseMove = m_MousePos - m_MousePosPrev;
            AnimateOrbit(deltaT, mouseMove);
            break;
        }
    case MouseState::Panning:
        AnimateTranslation(targetRotation);
        break;
    case MouseState::Idle:
        break;
    }

    const float3 vectorToCamera = -m_Distance * targetRotation.row2;
    const float3 camPos = m_TargetPos + vectorToCamera;

    m_CameraPos = camPos;
    m_CameraRight = -targetRotation.row0;
    m_CameraUp = targetRotation.row1;
    m_CameraDir = targetRotation.row2;
    UpdateWorldToView();
    
    m_MousePosPrev = m_MousePos;
}

void ThirdPersonCamera::LookAt(dm::float3 cameraPos, dm::float3 cameraTarget)
{
    dm::float3 cameraDir = cameraTarget - cameraPos;

    float azimuth, elevation, dirLength;
    dm::cartesianToSpherical(cameraDir, azimuth, elevation, dirLength);

    SetTargetPosition(cameraTarget);
    SetDistance(dirLength);
    azimuth = -(azimuth + dm::PI_f * 0.5f);
    SetRotation(azimuth, elevation);
}

void ThirdPersonCamera::LookTo(dm::float3 cameraPos, dm::float3 cameraDir,
    std::optional<float> targetDistance)
{
    float azimuth, elevation, dirLength;
    dm::cartesianToSpherical(-cameraDir, azimuth, elevation, dirLength);
    cameraDir /= dirLength;

    float const distance = targetDistance.value_or(GetDistance());
    SetTargetPosition(cameraPos + cameraDir * distance);
    SetDistance(distance);
    azimuth = -(azimuth + dm::PI_f * 0.5f);
    SetRotation(azimuth, elevation);
}

BaseCamera* SwitchableCamera::GetActiveUserCamera()
{
    if (IsFirstPersonActive())
        return &m_FirstPerson;

    if (IsThirdPersonActive())
        return &m_ThirdPerson;

    return nullptr;
}

BaseCamera const* SwitchableCamera::GetActiveUserCamera() const
{
    if (IsFirstPersonActive())
        return &m_FirstPerson;

    if (IsThirdPersonActive())
        return &m_ThirdPerson;

    return nullptr;
}

dm::affine3 SwitchableCamera::GetWorldToViewMatrix() const
{
    if (m_SceneCamera)
        return m_SceneCamera->GetWorldToViewMatrix();

    return GetActiveUserCamera()->GetWorldToViewMatrix();
}

bool SwitchableCamera::GetSceneCameraProjectionParams(float& verticalFov, float& zNear) const
{
    auto perspectiveCamera = std::dynamic_pointer_cast<engine::PerspectiveCamera>(m_SceneCamera);
    if (perspectiveCamera)
    {
        zNear = perspectiveCamera->zNear;
        verticalFov = perspectiveCamera->verticalFov;
        return true;
    }
    return false;
}

void SwitchableCamera::SwitchToFirstPerson(bool copyView)
{
    if (IsFirstPersonActive())
        return;

    if (copyView)
    {
        if (m_SceneCamera)
        {
            dm::affine3 viewToWorld = m_SceneCamera->GetViewToWorldMatrix();
            m_FirstPerson.LookTo(viewToWorld.m_translation, viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
        }
        else
        {
            m_FirstPerson.LookTo(m_ThirdPerson.GetPosition(), m_ThirdPerson.GetDir(), m_ThirdPerson.GetUp());
        }
    }

    m_UseFirstPerson = true;
    m_SceneCamera = nullptr;
}

void SwitchableCamera::SwitchToThirdPerson(bool copyView, std::optional<float> targetDistance)
{
    if (IsThirdPersonActive())
        return;
        
    if (copyView)
    {
        if (m_SceneCamera)
        {
            dm::affine3 viewToWorld = m_SceneCamera->GetViewToWorldMatrix();
            m_ThirdPerson.LookTo(viewToWorld.m_translation, viewToWorld.m_linear.row2, targetDistance);
        }
        else
        {
            m_ThirdPerson.LookTo(m_FirstPerson.GetPosition(), m_FirstPerson.GetDir(), targetDistance);
        }
    }

    m_UseFirstPerson = false;
    m_SceneCamera = nullptr;
}

void SwitchableCamera::SwitchToSceneCamera(std::shared_ptr<engine::SceneCamera> const& sceneCamera)
{
    assert(!!sceneCamera);
    
    m_SceneCamera = sceneCamera;
}

bool SwitchableCamera::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->KeyboardUpdate(key, scancode, action, mods);
        return true;
    }
    return false;
}

bool SwitchableCamera::MousePosUpdate(double xpos, double ypos)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->MousePosUpdate(xpos, ypos);
        return true;
    }
    return false;
}

bool SwitchableCamera::MouseButtonUpdate(int button, int action, int mods)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->MouseButtonUpdate(button, action, mods);
        return true;
    }
    return false;
}

bool SwitchableCamera::MouseScrollUpdate(double xoffset, double yoffset)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->MouseScrollUpdate(xoffset, yoffset);
        return true;
    }
    return false;
}

bool SwitchableCamera::JoystickButtonUpdate(int button, bool pressed)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->JoystickButtonUpdate(button, pressed);
        return true;
    }
    return false;
}

bool SwitchableCamera::JoystickUpdate(int axis, float value)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->JoystickUpdate(axis, value);
        return true;
    }
    return false;
}

void SwitchableCamera::Animate(float deltaT)
{
    BaseCamera* activeCamera = GetActiveUserCamera();
    if (activeCamera)
    {
        activeCamera->Animate(deltaT);
    }
}
