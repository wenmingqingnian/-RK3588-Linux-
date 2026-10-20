#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>

#include <cstdio>

#include "camera_router.h"

class RearRequestProbe : public QObject
{
    Q_OBJECT

public:
    int count = 0;
    bool lastActive = false;

public slots:
    void observe(bool active)
    {
        ++count;
        lastActive = active;
    }
};

static void expectTrue(bool condition, const char *message, int *failures)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", message);
    ++(*failures);
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    int failures = 0;
    vehiclecamera::CameraRouter router;
    RearRequestProbe probe;

    const QMetaObject::Connection connection =
        QObject::connect(&router,
                         SIGNAL(rearCaptureRequested(bool)),
                         &probe,
                         SLOT(observe(bool)));

    expectTrue(static_cast<bool>(connection),
               "CameraRouter exports rearCaptureRequested(bool)",
               &failures);

    router.setAvailable(vehiclecamera::CameraRole::Front, true);
    router.setReverseActive(true);

    expectTrue(router.displayRole() ==
                   vehiclecamera::CameraRole::Front,
               "Front remains displayed until the first Rear frame",
               &failures);
    expectTrue(probe.count == 1 && probe.lastActive,
               "REVERSE ON requests Rear capture exactly once",
               &failures);

    router.setReverseActive(true);
    expectTrue(probe.count == 1,
               "duplicate REVERSE ON is idempotent",
               &failures);

    router.setAvailable(vehiclecamera::CameraRole::Rear, true);
    expectTrue(router.displayRole() ==
                   vehiclecamera::CameraRole::Rear,
               "Rear is displayed only after it becomes available",
               &failures);

    router.setReverseActive(false);
    expectTrue(router.displayRole() ==
                   vehiclecamera::CameraRole::Front,
               "REVERSE OFF switches display to Front first",
               &failures);
    expectTrue(probe.count == 2 && !probe.lastActive,
               "REVERSE OFF requests Rear capture stop exactly once",
               &failures);

    router.setReverseActive(false);
    expectTrue(probe.count == 2,
               "duplicate REVERSE OFF is idempotent",
               &failures);

    if (failures != 0) {
        std::fprintf(stderr, "TEST RESULT: FAIL (%d failures)\n", failures);
        return 1;
    }

    std::printf("TEST RESULT: PASS\n");
    return 0;
}

#include "test_camera_router_lifecycle.moc"
