// AK820 Knob -> ESP LED Controller (Qt6 / Debian Linux) - System Tray Edition
//
// Behavior (same as the Windows build):
//   Ctrl + Knob            -> Warm  (Ctrl+Shift+Knob -> White), unless swapped
//   Ctrl + Press           -> Swap which combo controls Warm vs White
//                             (also cancels "Both" mode if active)
//   Ctrl + Long Press      -> Power On/Off (fires while still held).
//                             Turning Ctrl+Knob while off auto-wakes the LEDs.
//   Ctrl + Double Press    -> "Both" mode for BOTH_MODE_DURATION_MS
//                             (Ctrl+Knob drives Warm and White together).
//
// Linux specifics:
//   - HID: hidapi (hidraw backend), read on a QThread, events go to the GUI
//     thread via queued signals.
//   - Volume conflict (Wayland): EVIOCGRAB + uinput clone drops volume keys
//     while Ctrl is held (see EvdevVolumeFilter).
//   - Volume conflict (Xorg): XGrabKey on Ctrl+VolUp/VolDown/Mute (and Ctrl+Shift,
//     plus NumLock/CapsLock variants) so the desktop never sees them.
//     Plain volume keys are untouched.
//   - Modifier state: evdev EVIOCGKEY (works on Wayland and Xorg).
//   - HTTP: async QNetworkAccessManager, 1s timeout.
//   - Autostart: ~/.config/autostart/ak820-controller.desktop
//
// Wayland: no global key grab exists, so Ctrl+Knob may also reach the desktop's
// own volume handling (GNOME doesn't bind Ctrl+Volume by default). On Xorg the
// grab swallows it.

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QLockFile>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <hidapi/hidapi.h>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>

#include <memory>
#include <vector>

// X11 headers last (they #define junk like None/Bool that clash with Qt)
#include <X11/Xlib.h>
#include <X11/XF86keysym.h>
#include <X11/keysym.h>

// X11 macros that collide with Qt's moc-generated code (QMetaType::Bool etc.)
#undef Bool
#undef None
#undef Status
#undef Always
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut

// ------------------------- Config -------------------------

static const QString ESP_IP = QStringLiteral("192.168.0.212");

constexpr int STEP_FIRST = 2;
constexpr int STEP_SLOW  = 1;
constexpr int STEP_FAST  = 5;

constexpr int FAST_THRESHOLD_MS  = 120;
constexpr int PAUSE_THRESHOLD_MS = 400;

constexpr int CODE_VOL_UP = 233;
constexpr int CODE_VOL_DN = 234;
constexpr int CODE_MUTE   = 226;
constexpr int CODE_NONE   = 0;

constexpr unsigned short VID = 0x320F;
constexpr unsigned short PID = 0x505B;

constexpr int LONG_PRESS_MS          = 600;
constexpr int DOUBLE_PRESS_WINDOW_MS = 1000;
constexpr int BOTH_MODE_DURATION_MS  = 30000;

// ------------------------- X11 helpers -------------------------

namespace xh {
static int ignoreXError(Display*, XErrorEvent*) { return 0; }
} // namespace xh

// Global Ctrl/Shift state straight from the kernel (evdev) - works on Wayland
// and Xorg. Equivalent of GetAsyncKeyState. Needs read access to the keyboard's
// /dev/input/event* node (see 70-ak820.rules). Used from the HID thread only.
class KeyState {
public:
    ~KeyState() { for (int fd : fds) ::close(fd); }
    bool ctrl()  { return held(KEY_LEFTCTRL)  || held(KEY_RIGHTCTRL); }
    bool shift() { return held(KEY_LEFTSHIFT) || held(KEY_RIGHTSHIFT); }

private:
    std::vector<int> fds;
    QElapsedTimer lastScan;
    bool scanned = false;

    static bool hasBit(const unsigned char* bits, int n) { return bits[n / 8] & (1 << (n % 8)); }

    void scan() {
        scanned = true;
        lastScan.restart();
        for (int fd : fds) ::close(fd);
        fds.clear();
        const auto entries = QDir("/dev/input").entryList({ "event*" }, QDir::System);
        for (const QString& e : entries) {
            QByteArray path = ("/dev/input/" + e).toLocal8Bit();
            int fd = ::open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned char ev[(EV_MAX + 8) / 8] = {};
            unsigned char key[(KEY_MAX + 8) / 8] = {};
            if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !hasBit(ev, EV_KEY) ||
                ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0 || !hasBit(key, KEY_LEFTCTRL)) {
                ::close(fd);
                continue;
            }
            fds.push_back(fd);
        }
    }

    bool held(int code) {
        // (Re)scan if nothing is open yet - covers hotplug / late permission fixes
        if (!scanned || (fds.empty() && lastScan.elapsed() > 2000)) scan();
        unsigned char bits[(KEY_MAX + 8) / 8];
        for (size_t i = 0; i < fds.size();) {
            memset(bits, 0, sizeof(bits));
            if (ioctl(fds[i], EVIOCGKEY(sizeof(bits)), bits) < 0) {
                ::close(fds[i]);
                fds.erase(fds.begin() + i);
                continue;
            }
            if (hasBit(bits, code)) return true;
            ++i;
        }
        return false;
    }
};

// Swallows Ctrl+Volume keys system-wide (equivalent of the WH_KEYBOARD_LL hook)
class VolumeKeyGrabber : public QObject {
public:
    explicit VolumeKeyGrabber(QObject* parent = nullptr) : QObject(parent) {
        d = XOpenDisplay(nullptr);
        if (!d) return;

        XSetErrorHandler(xh::ignoreXError);
        Window root = DefaultRootWindow(d);

        const KeySym syms[] = { XF86XK_AudioRaiseVolume, XF86XK_AudioLowerVolume, XF86XK_AudioMute };
        const unsigned base[] = { ControlMask, ControlMask | ShiftMask };
        const unsigned lock[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

        for (KeySym s : syms) {
            KeyCode kc = XKeysymToKeycode(d, s);
            if (!kc) continue;
            for (unsigned b : base)
                for (unsigned l : lock)
                    XGrabKey(d, kc, b | l, root, False, GrabModeAsync, GrabModeAsync);
        }
        XSync(d, False);

        // Drain grabbed events so Xlib's queue doesn't grow forever
        notifier = new QSocketNotifier(ConnectionNumber(d), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] {
            while (XPending(d)) { XEvent ev; XNextEvent(d, &ev); }
        });
    }
    ~VolumeKeyGrabber() override { if (d) XCloseDisplay(d); }
    bool ok() const { return d != nullptr; }

private:
    Display* d = nullptr;
    QSocketNotifier* notifier = nullptr;
};

// Wayland-safe replacement for the Windows keyboard hook.
// Grabs (EVIOCGRAB) the AK820 input device(s) that can emit volume keys,
// re-injects everything through a uinput clone, and drops volume keys that
// start while Ctrl is held. If the app exits/crashes the kernel releases the
// grab and destroys the clone, so the keyboard is never left dead.
// Devices with LEDs (the plain boot keyboard) are skipped so Caps Lock LED sync
// keeps working; the knob's consumer-control lives on the other interface.
static inline bool testBit(const unsigned char* b, int n) { return b[n / 8] & (1 << (n % 8)); }

class EvdevVolumeFilter : public QThread {
    Q_OBJECT
signals:
    void failed(const QString& why);

protected:
    void run() override {
        KeyState ks;
        QElapsedTimer scanTimer;
        bool first = true, warned = false;

        while (!isInterruptionRequested()) {
            if (first || scanTimer.elapsed() > 2000) {
                first = false;
                scanTimer.restart();
                scan(warned);
            }
            if (devs.empty()) { msleep(500); continue; }

            std::vector<pollfd> pfds;
            for (const Dev& d : devs) pfds.push_back({ d.in, POLLIN, 0 });
            if (poll(pfds.data(), pfds.size(), 200) <= 0) continue;

            for (size_t i = pfds.size(); i-- > 0;) {
                if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) continue;
                if (!pump(devs[i], ks)) {
                    destroy(devs[i]);
                    devs.erase(devs.begin() + i);
                }
            }
        }
        for (Dev& d : devs) destroy(d);
        devs.clear();
    }

private:
    struct Dev {
        int in = -1, out = -1;
        bool sup[3] = { false, false, false }; // mute, vol-down, vol-up
        QByteArray path;
    };
    std::vector<Dev> devs;

    static int volIdx(int code) {
        if (code == KEY_MUTE) return 0;
        if (code == KEY_VOLUMEDOWN) return 1;
        if (code == KEY_VOLUMEUP) return 2;
        return -1;
    }

    void scan(bool& warned) {
        const auto entries = QDir("/dev/input").entryList({ "event*" }, QDir::System);
        for (const QString& e : entries) {
            QByteArray path = ("/dev/input/" + e).toLocal8Bit();
            bool known = false;
            for (const Dev& d : devs) if (d.path == path) { known = true; break; }
            if (known) continue;
            QString err;
            if (!tryAdd(path, err) && !err.isEmpty() && !warned) {
                warned = true;
                qWarning("Volume suppression unavailable: %s", qPrintable(err));
                emit failed(err);
            }
        }
    }

    // returns true if added; err stays empty when the device is simply not ours
    bool tryAdd(const QByteArray& path, QString& err) {
        int in = ::open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (in < 0) return false;

        input_id id{};
        unsigned char evb[(EV_MAX + 8) / 8] = {};
        unsigned char key[(KEY_MAX + 8) / 8] = {};
        char name[128] = {};
        if (ioctl(in, EVIOCGID, &id) < 0 || id.vendor != VID || id.product != PID ||
            ioctl(in, EVIOCGBIT(0, sizeof(evb)), evb) < 0 || !testBit(evb, EV_KEY) ||
            ioctl(in, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0 || !testBit(key, KEY_VOLUMEUP) ||
            testBit(evb, EV_LED)) {
            ::close(in);
            return false;
        }
        ioctl(in, EVIOCGNAME(sizeof(name)), name);

        int out = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (out < 0) {
            ::close(in);
            err = QStringLiteral("cannot open /dev/uinput (%1). Load the module and install the udev rule.")
                      .arg(strerror(errno));
            return false;
        }

        ioctl(out, UI_SET_EVBIT, EV_SYN);
        const int types[] = { EV_KEY, EV_REL, EV_ABS, EV_MSC, EV_SW }; // no EV_REP: source already repeats
        const int maxes[] = { KEY_MAX, REL_MAX, ABS_MAX, MSC_MAX, SW_MAX };
        const unsigned long setReq[] = { UI_SET_KEYBIT, UI_SET_RELBIT, UI_SET_ABSBIT, UI_SET_MSCBIT, UI_SET_SWBIT };
        for (int t = 0; t < 5; ++t) {
            if (!testBit(evb, types[t])) continue;
            unsigned char b[(KEY_MAX + 8) / 8] = {};
            if (ioctl(in, EVIOCGBIT(types[t], sizeof(b)), b) < 0) continue;
            ioctl(out, UI_SET_EVBIT, types[t]);
            for (int c = 0; c <= maxes[t]; ++c) {
                if (!testBit(b, c)) continue;
                ioctl(out, setReq[t], c);
                if (types[t] == EV_ABS) {
                    uinput_abs_setup as{};
                    as.code = c;
                    if (ioctl(in, EVIOCGABS(c), &as.absinfo) == 0) ioctl(out, UI_ABS_SETUP, &as);
                }
            }
        }

        uinput_setup us{};
        us.id.bustype = BUS_USB;
        us.id.vendor = VID;
        us.id.product = 0x0820; // != PID so we never grab our own clone
        snprintf(us.name, sizeof(us.name), "%s (filtered)", name);
        if (ioctl(out, UI_DEV_SETUP, &us) < 0 || ioctl(out, UI_DEV_CREATE) < 0) {
            err = QStringLiteral("uinput device creation failed (%1)").arg(strerror(errno));
            ::close(out); ::close(in);
            return false;
        }
        if (ioctl(in, EVIOCGRAB, 1) < 0) {
            err = QStringLiteral("EVIOCGRAB failed (%1)").arg(strerror(errno));
            ioctl(out, UI_DEV_DESTROY);
            ::close(out); ::close(in);
            return false;
        }

        Dev d;
        d.in = in; d.out = out; d.path = path;
        devs.push_back(d);
        return true;
    }

    // false => device gone, drop it
    bool pump(Dev& d, KeyState& ks) {
        input_event buf[64], outb[64];
        for (;;) {
            ssize_t n = ::read(d.in, buf, sizeof(buf));
            if (n < 0) return errno == EAGAIN;
            if (n == 0) return false;
            int cnt = int(n / sizeof(input_event)), m = 0;
            for (int i = 0; i < cnt; ++i) {
                const input_event& ev = buf[i];
                if (ev.type == EV_KEY) {
                    int vi = volIdx(ev.code);
                    if (vi >= 0) {
                        if (ev.value == 1) d.sup[vi] = ks.ctrl();
                        bool drop = d.sup[vi];
                        if (ev.value == 0) d.sup[vi] = false;
                        if (drop) continue; // swallow press/repeat/release
                    }
                }
                outb[m++] = ev;
            }
            if (m) { ssize_t w = ::write(d.out, outb, m * sizeof(input_event)); (void)w; }
        }
    }

    static void destroy(Dev& d) {
        ioctl(d.in, EVIOCGRAB, 0);
        ioctl(d.out, UI_DEV_DESTROY);
        ::close(d.out);
        ::close(d.in);
    }
};

// ------------------------- HID thread -------------------------

static hid_device* openConsumerInterface() {
    hid_device_info* devs = hid_enumerate(VID, PID);
    hid_device_info* target = nullptr;
    for (hid_device_info* p = devs; p; p = p->next) {
        if (p->usage_page == 0x0C) { target = p; break; }
    }
    hid_device* dev = target ? hid_open_path(target->path) : nullptr;
    if (devs) hid_free_enumeration(devs);
    return dev;
}

class HidWorker : public QThread {
    Q_OBJECT
signals:
    void rotated(int dir, bool shift);
    void buttonDown(bool ctrl);
    void buttonUp();

protected:
    void run() override {
        hid_device* dev = nullptr;
        unsigned char buf[8];
        int prevCode = -1;
        KeyState ks;
        bool btnDown = false;
        bool btnCtrl = false;

        while (!isInterruptionRequested()) {
            if (!dev) {
                dev = openConsumerInterface();
                if (!dev) { msleep(1000); continue; }
            }

            int res = hid_read_timeout(dev, buf, sizeof(buf), 100);
            if (res < 0) {
                hid_close(dev);
                dev = nullptr;
                prevCode = -1;
                msleep(1000);
                continue;
            }
            if (res < 2) continue;

            int code = buf[1];
            if (code == prevCode) continue;
            prevCode = code;

            if (code == CODE_NONE) {
                if (btnDown) {
                    btnDown = false;
                    if (btnCtrl) emit buttonUp();
                }
                continue;
            }

            if (code == CODE_MUTE) {
                btnDown = true;
                btnCtrl = ks.ctrl();
                if (btnCtrl) emit buttonDown(true);
                continue;
            }

            if (!ks.ctrl()) continue; // not our combo

            if (code == CODE_VOL_UP)      emit rotated(+1, ks.shift());
            else if (code == CODE_VOL_DN) emit rotated(-1, ks.shift());
        }

        if (dev) hid_close(dev);
    }
};

// ------------------------- Controller (GUI thread) -------------------------

class Controller : public QObject {
    Q_OBJECT
public:
    Controller() {
        tray.setIcon(makeDotIcon(true, false));
        tray.setToolTip(QStringLiteral("AK820 Controller"));

        longPressTimer.setSingleShot(true);
        longPressTimer.setInterval(LONG_PRESS_MS);
        connect(&longPressTimer, &QTimer::timeout, this, [this] {
            if (btnDown) { longPressFired = true; onLongPress(); }
        });

        clickClearTimer.setSingleShot(true);
        clickClearTimer.setInterval(DOUBLE_PRESS_WINDOW_MS);
        connect(&clickClearTimer, &QTimer::timeout, this, [this] { pendingSingleClick = false; });

        bothTimer.setSingleShot(true);
        bothTimer.setInterval(BOTH_MODE_DURATION_MS);
        connect(&bothTimer, &QTimer::timeout, this, [this] {
            bothMode = false;
            updateTray();
            balloon(QStringLiteral("Both Mode"), QStringLiteral("Expired."));
        });

        buildMenu();
        connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::DoubleClick) onLongPress();
        });

        // Read current state from the ESP (keyboard doesn't need to be connected)
        fetchPercent(3, [this](int v) { warm = savedWarm = v; updateTray(); });
        fetchPercent(4, [this](int v) { white = savedWhite = v; updateTray(); });

        tray.show();
        updateTray();
        balloon(QStringLiteral("AK820 Controller"),
                QStringLiteral("Running in the system tray. Right-click the icon for options."));

        QSettings s;
        if (!s.value("AutostartConfigured", false).toBool()) {
            setAutostart(true);
            s.setValue("AutostartConfigured", true);
        }
    }

public slots:
    void notify(const QString& title, const QString& text) { balloon(title, text); }

    void onRotate(int dir, bool shift) {
        bool stateChanged = false;

        if (!ledsOn) {
            warm = setLed(3, savedWarm);
            white = setLed(4, savedWhite);
            ledsOn = true;
            stateChanged = true;
            balloon(QStringLiteral("Power"), QStringLiteral("LEDs ON (auto, via knob)"));
        }

        int step = computeStep(dir);

        if (bothMode) {
            warm = setLed(3, warm + dir * step);
            white = setLed(4, white + dir * step);
        } else {
            bool wantWhite = shift;
            if (toggled) wantWhite = !wantWhite;
            if (wantWhite) white = setLed(4, white + dir * step);
            else           warm  = setLed(3, warm + dir * step);
        }

        if (stateChanged) refreshIcon();
        refreshTooltip();
    }

    void onButtonDown(bool ctrl) {
        btnDown = true;
        longPressFired = false;
        if (ctrl) longPressTimer.start();
    }

    void onButtonUp() {
        if (!btnDown) return;
        btnDown = false;
        longPressTimer.stop();
        if (!longPressFired) onShortPress();
    }

private:
    // ---- state ----
    int warm = 0, white = 0;
    int savedWarm = 0, savedWhite = 0;
    bool toggled = false;
    bool ledsOn = true;
    bool bothMode = false;

    bool btnDown = false;
    bool longPressFired = false;
    bool pendingSingleClick = false;

    QElapsedTimer lastRotate;
    int lastRotateDir = 0;
    bool hasLastRotate = false;

    QTimer longPressTimer, clickClearTimer, bothTimer;

    QSystemTrayIcon tray;
    QMenu menu;
    QAction* statusAct = nullptr;
    QAction* swapAct = nullptr;
    QAction* powerAct = nullptr;
    QAction* bothAct = nullptr;
    QAction* autostartAct = nullptr;

    QNetworkAccessManager net;

    // ---- HTTP ----
    QNetworkReply* httpGet(const QString& pathAndQuery) {
        QNetworkRequest req(QUrl(QStringLiteral("http://%1%2").arg(ESP_IP, pathAndQuery)));
        req.setTransferTimeout(1000);
        return net.get(req);
    }

    void fetchPercent(int led, std::function<void(int)> cb) {
        QNetworkReply* r = httpGet(QStringLiteral("/getPWM?led=%1").arg(led));
        connect(r, &QNetworkReply::finished, this, [r, cb] {
            int pct = 0;
            if (r->error() == QNetworkReply::NoError) {
                bool ok = false;
                int pwm = r->readAll().trimmed().toInt(&ok);
                if (ok) pct = (int)std::lround(pwm * 100.0 / 255.0);
            }
            r->deleteLater();
            cb(pct);
        });
    }

    int setLed(int led, int value) {
        value = std::max(0, std::min(100, value));
        QNetworkReply* r = httpGet(QStringLiteral("/setPWM?led=%1&value=%2").arg(led).arg(value));
        connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        return value;
    }

    // ---- autostart ----
    static QString autostartPath() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
               + "/autostart/ak820-controller.desktop";
    }
    static bool isAutostartEnabled() { return QFile::exists(autostartPath()); }
    static void setAutostart(bool enable) {
        QString p = autostartPath();
        if (!enable) { QFile::remove(p); return; }
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream o(&f);
            o << "[Desktop Entry]\nType=Application\nName=AK820 Controller\n"
              << "Exec=\"" << QCoreApplication::applicationFilePath() << "\"\n"
              << "X-GNOME-Autostart-enabled=true\nTerminal=false\n";
        }
    }

    // ---- tray ----
    static QIcon makeDotIcon(bool on, bool both) {
        const int size = 32;
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(4, 4, size - 8, size - 8);

        if (!on) {
            p.setBrush(QColor(90, 90, 90));
            p.setPen(QPen(QColor(60, 60, 60), 2));
            p.drawEllipse(r);
        } else if (both) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 176, 59));
            p.drawPie(r, 90 * 16, 180 * 16);
            p.setBrush(QColor(214, 236, 255));
            p.drawPie(r, 270 * 16, 180 * 16);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(200, 200, 200), 1));
            p.drawEllipse(r);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 176, 59));
            p.drawEllipse(r);
        }
        p.end();
        return QIcon(pm);
    }

    void balloon(const QString& title, const QString& text) {
        tray.showMessage(title, text, QSystemTrayIcon::Information, 2500);
    }

    void refreshTooltip() {
        QString tip = QStringLiteral("AK820 Controller\nWarm %1%  |  White %2%\n%3")
            .arg(warm).arg(white)
            .arg(toggled ? "Ctrl->White, Ctrl+Shift->Warm" : "Ctrl->Warm, Ctrl+Shift->White");
        if (bothMode) tip += "\nBoth Mode active";
        if (!ledsOn) tip += "\n(Off)";
        tray.setToolTip(tip);
    }

    void refreshIcon() { tray.setIcon(makeDotIcon(ledsOn, bothMode)); }

    void updateTray() { refreshIcon(); refreshTooltip(); }

    void buildMenu() {
        statusAct = menu.addAction(QString());
        statusAct->setEnabled(false);
        menu.addSeparator();

        swapAct = menu.addAction(QString(), this, [this] { onShortPress(); });
        powerAct = menu.addAction(QString(), this, [this] { onLongPress(); });
        bothAct = menu.addAction(QString(), this, [this] {
            if (bothMode) cancelBoth(QStringLiteral("Cancelled."));
            else activateBothMode();
        });
        bothAct->setCheckable(true);

        menu.addSeparator();
        autostartAct = menu.addAction(QStringLiteral("Start with Login"), this, [this] {
            bool now = !isAutostartEnabled();
            setAutostart(now);
            QSettings().setValue("AutostartConfigured", true);
            balloon(QStringLiteral("Startup"),
                    now ? QStringLiteral("AK820 Controller will start with login")
                        : QStringLiteral("Removed from startup"));
        });
        autostartAct->setCheckable(true);

        menu.addSeparator();
        menu.addAction(QStringLiteral("Exit"), qApp, &QApplication::quit);

        connect(&menu, &QMenu::aboutToShow, this, [this] {
            statusAct->setText(QStringLiteral("Warm %1%   |   White %2%").arg(warm).arg(white));
            swapAct->setText(QStringLiteral("Swap Warm/White  (Ctrl -> %1)").arg(toggled ? "White" : "Warm"));
            powerAct->setText(ledsOn ? "Turn Off" : "Turn On");
            bothAct->setChecked(bothMode);
            bothAct->setText(bothMode ? "Both Mode active - click to cancel" : "Activate Both Mode");
            autostartAct->setChecked(isAutostartEnabled());
        });
        tray.setContextMenu(&menu);
    }

    // ---- actions ----
    void activateBothMode() {
        bothMode = true;
        bothTimer.start();
        updateTray();
        balloon(QStringLiteral("Both Mode"),
                QStringLiteral("Active for %1s - Ctrl+Knob now moves Warm & White together.")
                    .arg(BOTH_MODE_DURATION_MS / 1000));
    }

    void cancelBoth(const QString& why) {
        bothMode = false;
        pendingSingleClick = false;
        bothTimer.stop();
        updateTray();
        balloon(QStringLiteral("Both Mode"), why);
    }

    void onShortPress() {
        if (bothMode) { cancelBoth(QStringLiteral("Cancelled by Ctrl+Press.")); return; }

        if (pendingSingleClick) {
            // Double press: 1st press already toggled optimistically, revert it
            pendingSingleClick = false;
            clickClearTimer.stop();
            toggled = !toggled;
            activateBothMode();
        } else {
            toggled = !toggled;
            updateTray();
            balloon(QStringLiteral("Mapping Changed"),
                    toggled ? QStringLiteral("Ctrl -> White, Ctrl+Shift -> Warm")
                            : QStringLiteral("Ctrl -> Warm, Ctrl+Shift -> White"));
            pendingSingleClick = true;
            clickClearTimer.start();
        }
    }

    void onLongPress() {
        if (ledsOn) {
            savedWarm = warm;
            savedWhite = white;
            warm = setLed(3, 0);
            white = setLed(4, 0);
            ledsOn = false;
            updateTray();
            balloon(QStringLiteral("Power"), QStringLiteral("LEDs OFF"));
        } else {
            warm = setLed(3, savedWarm);
            white = setLed(4, savedWhite);
            ledsOn = true;
            updateTray();
            balloon(QStringLiteral("Power"),
                    QStringLiteral("LEDs ON (Warm=%1 White=%2)").arg(warm).arg(white));
        }
    }

    int computeStep(int dir) {
        int step;
        if (!hasLastRotate || dir != lastRotateDir) {
            step = STEP_FIRST;
        } else {
            qint64 gap = lastRotate.elapsed();
            if (gap > PAUSE_THRESHOLD_MS)      step = STEP_FIRST;
            else if (gap < FAST_THRESHOLD_MS)  step = STEP_FAST;
            else                               step = STEP_SLOW;
        }
        lastRotate.restart();
        lastRotateDir = dir;
        hasLastRotate = true;
        return step;
    }
};

// ------------------------- Entry point -------------------------

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("AK820Controller");
    app.setOrganizationName("AK820Controller");
    app.setQuitOnLastWindowClosed(false);

    // Single instance guard
    QLockFile lock(QDir::temp().filePath("ak820-controller.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) return 0;

    if (hid_init() != 0) return 1;

    Controller controller;
    // Key grab only exists on Xorg; on Wayland there is no global grab API
    std::unique_ptr<VolumeKeyGrabber> grabber;
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == "x11")
        grabber = std::make_unique<VolumeKeyGrabber>();

    // Wayland: evdev grab + uinput filter does the volume suppression
    EvdevVolumeFilter filter;
    if (!grabber) {
        QObject::connect(&filter, &EvdevVolumeFilter::failed, &controller,
                         [&controller](const QString& why) {
                             controller.notify(QStringLiteral("Volume suppression off"), why);
                         });
        filter.start();
    }

    HidWorker worker;
    QObject::connect(&worker, &HidWorker::rotated,    &controller, &Controller::onRotate);
    QObject::connect(&worker, &HidWorker::buttonDown, &controller, &Controller::onButtonDown);
    QObject::connect(&worker, &HidWorker::buttonUp,   &controller, &Controller::onButtonUp);
    worker.start();

    int rc = app.exec();

    worker.requestInterruption();
    filter.requestInterruption();
    worker.wait();
    filter.wait();
    hid_exit();
    return rc;
}

#include "main.moc"