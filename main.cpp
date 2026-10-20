#include "mainwindow.h"
#include "dmesg_tail.h"
#include "logmanager.h"
#include <QApplication>
#include <QAbstractButton>
#include <QEvent>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QWidget>
#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <cstring>

// Signal-safe flag: set by SIGINT/SIGTERM handler, polled by QTimer.
// Handler only stores a value (async-signal-safe); Qt quit happens in
// the main thread via the timer to avoid calling Qt APIs from a signal handler.
static volatile std::sig_atomic_t g_signalReceived = 0;

static void signalHandler(int /*sig*/)
{
    g_signalReceived = 1;
}


class ChildPageNormalizer final : public QObject
{
public:
    explicit ChildPageNormalizer(QWidget *mainWindow)
        : QObject(nullptr)
        , mainWindow_(mainWindow)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        QWidget *window = qobject_cast<QWidget *>(watched);

        if (!isManagedPage(window)) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::Show) {
            QPointer<QWidget> guard(window);

            QTimer::singleShot(
                0,
                this,
                [this, guard]() {
                    if (guard) {
                        normalizePage(guard.data());
                    }
                }
            );
        } else if (event->type() == QEvent::Resize) {
            positionBackButton(window);
        }

        return QObject::eventFilter(watched, event);
    }

private:
    bool isManagedPage(QWidget *window) const
    {
        if (window == nullptr ||
            window == mainWindow_.data()) {
            return false;
        }

        static const QStringList managedClasses = {
            QStringLiteral("Weather"),
            QStringLiteral("BaiduMap"),
            QStringLiteral("Monitor"),
            QStringLiteral("MusicPlayer"),
            QStringLiteral("SearchMusic"),
            QStringLiteral("SettingWindow")
        };

        const QString className =
            QString::fromLatin1(
                window->metaObject()->className()
            );

        return managedClasses.contains(className);
    }

    QRect targetGeometry() const
    {
        if (mainWindow_ &&
            mainWindow_->size().isValid()) {
            return QRect(
                QPoint(0, 0),
                mainWindow_->size()
            );
        }

        QScreen *screen =
            QGuiApplication::primaryScreen();

        if (screen != nullptr) {
            return screen->geometry();
        }

        return QRect(0, 0, 1920, 1080);
    }

    QAbstractButton *findNativeBackButton(
        QWidget *window
    ) const
    {
        if (window == nullptr) {
            return nullptr;
        }

        const QList<QAbstractButton *> buttons =
            window->findChildren<QAbstractButton *>();

        for (QAbstractButton *button : buttons) {
            if (button == nullptr ||
                button->objectName() ==
                    QStringLiteral("ct_global_back_button")) {
                continue;
            }

            QString text = button->text();
            text.remove(QLatin1Char('&'));

            const bool isBackButton =
                text.contains(
                    QStringLiteral("返回"),
                    Qt::CaseInsensitive
                ) ||
                text.contains(
                    QStringLiteral("退出"),
                    Qt::CaseInsensitive
                ) ||
                text.contains(
                    QStringLiteral("关闭"),
                    Qt::CaseInsensitive
                ) ||
                text.contains(
                    QStringLiteral("back"),
                    Qt::CaseInsensitive
                );

            if (isBackButton && button->isEnabled()) {
                return button;
            }
        }

        return nullptr;
    }

    void dismissPage(QWidget *window)
    {
        if (window == nullptr) {
            return;
        }

        QAbstractButton *nativeBack =
            findNativeBackButton(window);

        if (nativeBack != nullptr) {
            nativeBack->click();
        } else {
            window->hide();
        }
    }

    QPushButton *ensureBackButton(QWidget *window)
    {
        QPushButton *button =
            window->findChild<QPushButton *>(
                QStringLiteral("ct_global_back_button"),
                Qt::FindDirectChildrenOnly
            );

        if (button != nullptr) {
            return button;
        }

        button = new QPushButton(
            QStringLiteral("返回主页"),
            window
        );

        button->setObjectName(
            QStringLiteral("ct_global_back_button")
        );

        button->setFixedSize(180, 68);
        button->setFocusPolicy(Qt::NoFocus);

        button->setStyleSheet(
            QStringLiteral(
                "QPushButton {"
                "background-color: rgba(20, 20, 20, 210);"
                "color: white;"
                "border: 2px solid white;"
                "border-radius: 10px;"
                "font-size: 26px;"
                "font-weight: bold;"
                "}"
                "QPushButton:pressed {"
                "background-color: rgba(0, 120, 215, 230);"
                "}"
            )
        );

        QPointer<QWidget> guard(window);

        QObject::connect(
            button,
            &QPushButton::clicked,
            this,
            [this, guard]() {
                if (!guard) {
                    return;
                }

                QAbstractButton *nativeBack =
                    findNativeBackButton(
                        guard.data()
                    );

                if (nativeBack != nullptr) {
                    /*
                     * Preserve each module's existing return
                     * behavior. Monitor's original handler can
                     * stop the camera and finish media work.
                     */
                    nativeBack->click();
                } else {
                    guard->hide();
                }

                if (mainWindow_) {
                    mainWindow_->show();
                    mainWindow_->raise();
                    mainWindow_->activateWindow();
                }
            }
        );

        button->show();
        return button;
    }

    void positionBackButton(QWidget *window)
    {
        if (window == nullptr) {
            return;
        }

        QPushButton *button =
            window->findChild<QPushButton *>(
                QStringLiteral("ct_global_back_button"),
                Qt::FindDirectChildrenOnly
            );

        if (button == nullptr) {
            return;
        }

        const int margin = 24;

        button->move(
            qMax(
                margin,
                window->width() -
                    button->width() -
                    margin
            ),
            margin
        );

        button->raise();
    }

    void normalizePage(QWidget *window)
    {
        if (window == nullptr ||
            window->property(
                "ct_page_normalizing"
            ).toBool()) {
            return;
        }

        window->setProperty(
            "ct_page_normalizing",
            true
        );

        /*
         * Only one module page may remain visible.
         * Use native back handlers where available.
         */
        const QWidgetList topLevels =
            QApplication::topLevelWidgets();

        for (QWidget *other : topLevels) {
            if (other == window ||
                !isManagedPage(other) ||
                !other->isVisible()) {
                continue;
            }

            dismissPage(other);
        }

        const QRect target = targetGeometry();

        /*
         * Remove old Designer-level 1024x600 limits
         * and force the page to the HDMI output size.
         */
        window->setMinimumSize(0, 0);
        window->setMaximumSize(
            QWIDGETSIZE_MAX,
            QWIDGETSIZE_MAX
        );

        window->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Expanding
        );

        window->setFixedSize(target.size());
        window->resize(target.size());
        window->move(target.topLeft());

        ensureBackButton(window);
        positionBackButton(window);

        window->raise();
        window->activateWindow();

        VLOG_INFO(
            "main",
            "normalizing child page %s to %dx%d",
            window->metaObject()->className(),
            target.width(),
            target.height()
        );

        QPointer<QWidget> guard(window);

        QTimer::singleShot(
            300,
            this,
            [this, guard, target]() {
                if (!guard ||
                    !guard->isVisible()) {
                    return;
                }

                guard->setFixedSize(target.size());
                guard->resize(target.size());
                guard->move(target.topLeft());

                positionBackButton(guard.data());

                VLOG_INFO(
                    "main",
                    "child page %s size after show: %dx%d",
                    guard->metaObject()->className(),
                    guard->width(),
                    guard->height()
                );
            }
        );

        window->setProperty(
            "ct_page_normalizing",
            false
        );
    }

    QPointer<QWidget> mainWindow_;
};

int main(int argc, char *argv[])
{
    // Enable virtual keyboard for touchscreen
    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));

    // Initialize logging
    vehiclelog::setMinLevel(vehiclelog::Level::Info);

    // Start dmesg tailer: kernel logs are forwarded to LogManager as
    // VLOG_INFO("kernel", ...). Non-Linux platforms return false and no-op.
    // RAII: tailer stops itself on scope exit; no explicit stop() needed.
    vehiclelog::DmesgTail dmesgTail;
    if (dmesgTail.start())
        VLOG_INFO("main", "dmesg tailer started (kernel logs will be tagged [kernel])");
    else
        VLOG_INFO("main", "dmesg tailer not started (non-Linux or dmesg unavailable)");

    // Install SIGINT/SIGTERM handlers for graceful shutdown (Ctrl+C / kill).
    // sa_flags=0 (no SA_RESTART) so blocking syscalls return EINTR.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        VLOG_ERROR("main", "sigaction(SIGINT) failed: %s", std::strerror(errno));
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        VLOG_ERROR("main", "sigaction(SIGTERM) failed: %s", std::strerror(errno));
    }

    QApplication app(argc, argv);

    VLOG_INFO("main", "application starting");

    try {
        MainWindow mainWindow;
        ChildPageNormalizer childPageNormalizer(&mainWindow);
        app.installEventFilter(&childPageNormalizer);
        QScreen *screen = QGuiApplication::primaryScreen();
        QSize targetSize(1920, 1080);

        if (screen != nullptr &&
            screen->geometry().size().isValid()) {
            targetSize = screen->geometry().size();
        } else {
            VLOG_WARN(
                "main",
                "primary screen geometry unavailable; using 1920x1080"
            );
        }

        /*
         * Do not request xdg-shell fullscreen or maximized state.
         * This Rockchip Weston build does not reliably resize this
         * Qt window for those protocol states.
         *
         * Use a normal frameless surface with the exact QScreen size.
         */
        mainWindow.setWindowFlags(
            Qt::Window | Qt::FramelessWindowHint
        );

        mainWindow.setFixedSize(targetSize);
        mainWindow.resize(targetSize);

        VLOG_INFO(
            "main",
            "forcing normal frameless window size: %dx%d",
            targetSize.width(),
            targetSize.height()
        );

        mainWindow.show();

        /*
         * Reapply after the first Wayland configure cycle so the
         * Designer startup geometry cannot become the final size.
         */
        QTimer::singleShot(
            500,
            &mainWindow,
            [&mainWindow, targetSize]() {
                mainWindow.setFixedSize(targetSize);
                mainWindow.resize(targetSize);

                VLOG_INFO(
                    "main",
                    "window size after show: %dx%d",
                    mainWindow.width(),
                    mainWindow.height()
                );
            }
        );

        VLOG_INFO("main", "application ready");

        // Poll for signal flag every 200ms; quit event loop when received.
        // QTimer ensures app.quit() is called on the main thread (Qt APIs
        // are not async-signal-safe and must not be called directly from
        // a signal handler).
        QTimer signalTimer;
        signalTimer.setInterval(200);
        QObject::connect(&signalTimer, &QTimer::timeout, [&app]() {
            if (g_signalReceived != 0) {
                VLOG_INFO("main", "SIGINT/SIGTERM received, quitting");
                app.quit();
            }
        });
        signalTimer.start();

        int exitCode = app.exec();

        VLOG_INFO("main", "application exiting (code=%d)", exitCode);
        return exitCode;
    } catch (const std::exception &e) {
        VLOG_ERROR("main", "fatal exception: %s", e.what());
        return 1;
    } catch (...) {
        VLOG_ERROR("main", "fatal unknown exception");
        return 1;
    }
}
