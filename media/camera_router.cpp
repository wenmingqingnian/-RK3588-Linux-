#include "camera_router.h"

namespace vehiclecamera {

const char *cameraRoleToString(CameraRole role)
{
    switch (role) {
    case CameraRole::Front:
        return "Front";
    case CameraRole::Rear:
        return "Rear";
    }
    return "Unknown";
}

CameraRouter::CameraRouter(QObject *parent)
    : QObject(parent)
{
}

CameraRole CameraRouter::displayRole() const
{
    return m_displayRole;
}

bool CameraRouter::reverseActive() const
{
    return m_reverseActive;
}

bool CameraRouter::isAvailable(CameraRole role) const
{
    return role == CameraRole::Front ? m_frontAvailable : m_rearAvailable;
}

void CameraRouter::setReverseActive(bool active)
{
    if (m_reverseActive == active)
        return;

    m_reverseActive = active;
    recomputeDisplayRole();
    emit rearCaptureRequested(active);
}

void CameraRouter::setAvailable(CameraRole role, bool available)
{
    bool *target = role == CameraRole::Front
        ? &m_frontAvailable
        : &m_rearAvailable;

    if (*target == available)
        return;

    *target = available;
    emit availabilityChanged(role, available);
    recomputeDisplayRole();
}

void CameraRouter::recomputeDisplayRole()
{
    const CameraRole desired =
        (m_reverseActive && m_rearAvailable)
            ? CameraRole::Rear
            : CameraRole::Front;

    const bool fallback = m_reverseActive && !m_rearAvailable;
    if (fallback != m_rearFallbackActive) {
        m_rearFallbackActive = fallback;
        emit rearFallbackChanged(fallback);
    }

    if (desired != m_displayRole) {
        m_displayRole = desired;
        emit displayRoleChanged(m_displayRole);
    }
}

} // namespace vehiclecamera
