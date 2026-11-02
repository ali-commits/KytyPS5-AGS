#ifndef AGS_SHELL_NAVIGATION_H
#define AGS_SHELL_NAVIGATION_H
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QSet>
#include <QTabWidget>

// Idle launcher routing only. The streaming owner suspends the shared sampler.
class ShellNavigation {
public:
    void Reset() { m_baseline = true; }
    template<class Library>
    void Handle(const QSet<QString>& pressed, QTabWidget* tabs, Library* library) {
        const auto edges = pressed - m_previous;
        m_previous = pressed;
        auto* root = tabs->window();
        auto* modal = QApplication::activeModalWidget();
        const bool ownedModal = modal && Owns(root, modal);
        auto* popup = QApplication::activePopupWidget();
        const bool ownedPopup = popup && Owns(root, popup);
        if (!root->isEnabled() || (!root->isActiveWindow() && !ownedModal && !ownedPopup)) { Reset(); library->ResetControllerNavigation(); return; }
        if (m_baseline) { m_baseline = false; library->ResetControllerNavigation(); library->HandleController(pressed); return; }
        if (popup && !Owns(root, popup) && popup->window() != root) return;
        if (!popup && !modal && (edges.contains("LeftShoulder") || edges.contains("RightShoulder"))) {
            const int delta = edges.contains("RightShoulder") ? 1 : -1;
            for (int step = 1; step <= tabs->count(); ++step) {
                const int index = (tabs->currentIndex() + delta * step + tabs->count()) % tabs->count();
                if (tabs->isTabVisible(index) && tabs->isTabEnabled(index)) { tabs->setCurrentIndex(index); break; }
            }
            return;
        }
        if (!popup && !modal && tabs->currentWidget() == library) { library->HandleController(pressed); return; }
        if (edges.contains("East")) {
            if (popup) Key(popup, Qt::Key_Escape);
            else if (ownedModal) Key(modal, Qt::Key_Escape);
            else if (!modal) tabs->setCurrentWidget(library);
            return;
        }
        if (modal && !ownedModal) return;
        auto* scope = popup ? popup : (ownedModal ? modal : tabs->currentWidget());
        auto* focus = QApplication::focusWidget();
        if (popup) {
            if (auto* view = popup->findChild<QAbstractItemView*>()) focus = view;
        }
        if (!focus || focus == scope || !scope->isAncestorOf(focus)) {
            focus = scope->nextInFocusChain();
            while (focus != scope && (!focus->isVisible() || !focus->isEnabled() || focus->focusPolicy() == Qt::NoFocus)) focus = focus->nextInFocusChain();
            if (focus == scope || !scope->isAncestorOf(focus)) return;
            focus->setFocus();
        }
        if (edges.contains("South")) {
            if (popup) Key(focus, Qt::Key_Return);
            else if (auto* button = qobject_cast<QAbstractButton*>(focus)) button->click();
            else if (auto* combo = qobject_cast<QComboBox*>(focus)) combo->showPopup();
            else if (qobject_cast<QAbstractItemView*>(focus)) Key(focus, Qt::Key_Return);
            return;
        }
        int key = 0;
        if (edges.contains("DpadUp")) key = Qt::Key_Up;
        if (edges.contains("DpadDown")) key = Qt::Key_Down;
        if (edges.contains("DpadLeft")) key = Qt::Key_Left;
        if (edges.contains("DpadRight")) key = Qt::Key_Right;
        if (!key) return;
        if (auto* spin = qobject_cast<QAbstractSpinBox*>(focus->parentWidget())) { Key(spin, key); return; }
        if (popup || qobject_cast<QAbstractItemView*>(focus) || qobject_cast<QAbstractSlider*>(focus) || qobject_cast<QAbstractSpinBox*>(focus)) Key(focus, key);
        else Key(focus, (key == Qt::Key_Up || key == Qt::Key_Left) ? Qt::Key_Backtab : Qt::Key_Tab);
    }
private:
    static bool Owns(QObject* root, QObject* child) {
        for (auto* object = child; object; object = object->parent()) if (object == root) return true;
        return false;
    }
    static void Key(QWidget* widget, int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier), release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &release);
    }
    QSet<QString> m_previous;
    bool m_baseline = true;
};
#endif
