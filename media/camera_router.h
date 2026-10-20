#ifndef VEHICLECAMERA_CAMERA_ROUTER_H
#define VEHICLECAMERA_CAMERA_ROUTER_H

#include <QObject>
#include <QMetaType>

namespace vehiclecamera {

enum class CameraRole {
    Front = 0,
    Rear = 1
};

const char *cameraRoleToString(CameraRole role);

class CameraRouter : public QObject
{
    Q_OBJECT
public:
    explicit CameraRouter(QObject *parent = nullptr);

    CameraRole displayRole() const;
    bool reverseActive() const;
    bool isAvailable(CameraRole role) const;

public slots:
    void setReverseActive(bool active);
    void setAvailable(CameraRole role, bool available);

signals:
    void displayRoleChanged(vehiclecamera::CameraRole role);
    void availabilityChanged(vehiclecamera::CameraRole role, bool available);
    void rearFallbackChanged(bool active);
    // Requests the owner of the Rear capture device to start or stop it.
    // Emitted only when the debounced REVERSE state actually changes.
    // Display selection remains independent from capture ownership.
    void rearCaptureRequested(bool active);

private:
    void recomputeDisplayRole();

    CameraRole m_displayRole = CameraRole::Front;
    bool m_reverseActive = false;
    bool m_frontAvailable = false;
    bool m_rearAvailable = false;
    bool m_rearFallbackActive = false;
};

} // namespace vehiclecamera

Q_DECLARE_METATYPE(vehiclecamera::CameraRole)

#endif // VEHICLECAMERA_CAMERA_ROUTER_H
