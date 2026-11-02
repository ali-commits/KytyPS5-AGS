#include "launcherTheme.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QPalette>
#include <QStyleHints>
#include <QTimer>

namespace {
QPalette ThemePalette(bool dark) {
    QPalette palette;
    const QColor window(dark ? "#17191e" : "#f6f6f6");
    const QColor text(dark ? "#f2f3f7" : "#202124");
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, QColor(dark ? "#22252b" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor(dark ? "#292d35" : "#eceff3"));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(dark ? "#292d35" : "#eceff3"));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, QColor(dark ? "#315f9f" : "#216cb0"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(dark ? "#91baff" : "#165fa6"));
    palette.setColor(QPalette::LinkVisited, QColor(dark ? "#c0a3ee" : "#7045a0"));
    palette.setColor(QPalette::Mid, QColor(dark ? "#454c59" : "#aab0ba"));
    palette.setColor(QPalette::Midlight, QColor(dark ? "#363d49" : "#dce1e8"));
    palette.setColor(QPalette::Light, QColor(dark ? "#555e6d" : "#ffffff"));
    palette.setColor(QPalette::Dark, QColor(dark ? "#101216" : "#7e8590"));
    palette.setColor(QPalette::PlaceholderText, QColor(dark ? "#aab1bf" : "#646c78"));
    for (auto role: {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(dark ? "#9399a6" : "#747b86"));
    return palette;
}

class ThemeController final: public QObject {
public:
    explicit ThemeController(QApplication& app): QObject(&app), application(app), systemPalette(app.palette()) {
        setObjectName("launcherThemeController");
        app.installEventFilter(this);
        connect(app.styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
            if (mode == 0) Apply(0);
        });
    }
    void Apply(int requested) {
        mode = requested >= 0 && requested <= 2 ? requested : 0;
        auto palette = systemPalette;
        const auto scheme = application.styleHints()->colorScheme();
        if (mode != 0 || scheme != Qt::ColorScheme::Unknown)
            palette = ThemePalette(mode == 1 || (mode == 0 && scheme == Qt::ColorScheme::Dark));
        applying = true;
        application.setPalette(palette);
        application.setStyleSheet(application.styleSheet());
        applying = false;
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == &application && event->type() == QEvent::ApplicationPaletteChange && !applying && !pending) {
            // Native palette notifications can arrive after startup or an OS theme change.
            // Keep that fallback separate from our explicit Dark/Light palettes.
            if (mode == 0) systemPalette = application.palette();
            pending = true;
            QTimer::singleShot(0, this, [this] { pending = false; Apply(mode); });
        }
        return QObject::eventFilter(watched, event);
    }
private:
    QApplication& application;
    QPalette systemPalette;
    int mode = 0;
    bool applying = false, pending = false;
};
ThemeController* Controller(QApplication& app) {
    for (auto* child: app.children())
        if (child->objectName() == "launcherThemeController") return static_cast<ThemeController*>(child);
    return new ThemeController(app);
}
} // namespace

namespace LauncherTheme {
void Initialize(QApplication& application) {
    Controller(application);
    QFile file(QStringLiteral(":/styles/launcher.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) application.setStyleSheet(QString::fromUtf8(file.readAll()));
}
void Apply(QApplication& application, int theme) { Controller(application)->Apply(theme); }
} // namespace LauncherTheme
