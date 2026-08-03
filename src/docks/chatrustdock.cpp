#include "chatrustdock.h"
#include "qmltypes/rustpanelitem.h"
#include "mainwindow.h"

#include <QKeySequence>
#include <QQuickWidget>
#include <QShortcut>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

ChatRustDock::ChatRustDock(QWidget *parent)
    : QDockWidget(parent)
{
    setObjectName("chatRustDock");
    setWindowTitle(tr("Chat (Rust)"));

    // QQuickWidget, not QQuickView + createWindowContainer: the latter
    // embeds a *separate native child window* in the widget tree, which
    // is a documented Qt flicker source during QMainWindow::restoreState()
    // dock reflows (the native child's own backing store gets
    // shown/hidden/resized out of sync with its parent widget). Reported
    // symptom ("black/white screen for a sub-second time" switching
    // Logging/Editing/FX/Color layouts) matches exactly: Qt Quick's
    // default clear color is white, this app's dark theme surface is
    // pure black (theme.rs's DARK.surface) -- the flash was that white
    // default briefly showing before real (black-background) content
    // painted. QQuickWidget renders into an offscreen buffer composited
    // by the normal widget painting system instead of a separate native
    // window, and exposes setClearColor() directly so the fallback color
    // matches the theme instead of Qt's white default.
    auto *view = new QQuickWidget();
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view->setClearColor(MainWindow::resolvedTheme() == "light" ? QColor(0xff, 0xff, 0xff)
                                                                : QColor(0x00, 0x00, 0x00));

    auto *panel = new RustPanelItem();
    // Keep the embedded item's size hint below the fresh-launch target.
    // QMainWindow's dock splitter can then grow it to the user's chosen
    // width without the QQuickWindow forcing a 400px minimum.
    panel->setWidth(240);
    panel->setHeight(260);
    view->setContent(QUrl(), nullptr, panel);

    // Phase 4 (chat-panel-ui-theme-parity.md): lowered from 280x300 so a
    // ~20%-of-window default width target isn't clamped back up on a
    // smaller monitor -- no maximum size is set (never was), so the dock
    // can still grow past its initial width via the native splitter drag
    // MainWindow::MainWindow's `resizeDocks(...)` call sets up.
    view->setMinimumSize(240, 260);

    setWidget(view);
    m_panel = panel;

    // changeTheme() already ran (statically, in main.cpp before
    // MainWindow exists) by the time this dock is constructed -- apply
    // its resolved result once now instead of waiting on a live signal
    // that doesn't exist in this app (theme switches require a restart,
    // see MainWindow::restartAfterChangeTheme()).
    applyTheme(MainWindow::resolvedTheme());

    // Window-wide so they fire regardless of which widget in the main
    // window currently has focus -- see the header comment. Checked against
    // Shotcut's own action shortcut table when this plan was written; none
    // used Ctrl+Alt+Up/Down or bare Ctrl+K.
    m_previousThreadShortcut = new QShortcut(QKeySequence("Ctrl+Alt+Up"), this);
    m_previousThreadShortcut->setContext(Qt::WindowShortcut);
    connect(m_previousThreadShortcut, &QShortcut::activated, this,
            [this]() { invokePanelCommand(RustPanelItem::PreviousThread); });

    m_nextThreadShortcut = new QShortcut(QKeySequence("Ctrl+Alt+Down"), this);
    m_nextThreadShortcut->setContext(Qt::WindowShortcut);
    connect(m_nextThreadShortcut, &QShortcut::activated, this,
            [this]() { invokePanelCommand(RustPanelItem::NextThread); });

    m_openSearchShortcut = new QShortcut(QKeySequence("Ctrl+K"), this);
    m_openSearchShortcut->setContext(Qt::WindowShortcut);
    connect(m_openSearchShortcut, &QShortcut::activated, this,
            [this]() { invokePanelCommand(RustPanelItem::OpenThreadSearch); });
}

void ChatRustDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);

    // First-dock click-dead-until-redock fix. Reported live on a real
    // (non-VNC) Windows install: Settings (and likely other panel
    // controls) don't respond to clicks the very first time this dock
    // lands in its default startup position, but a manual float+redock
    // fixes it immediately.
    //
    // This dock's *entire* initial placement --
    // MainWindow::setupAndConnectDocks()'s addDockWidget/splitDockWidget
    // calls, plus readWindowSettings()'s restoreState()/addDockWidget()
    // migration dance (mainwindow.cpp) -- runs inside MainWindow's own
    // constructor, before MainWindow is ever shown. No QResizeEvent from
    // a real window-manager-driven layout pass has reached the embedded
    // QQuickWidget by that point.
    //
    // The embedded RustPanelItem (rustpanelitem.cpp) paints correctly
    // regardless, because paint() always re-reads this item's *current*
    // width()/height() at paint time. But its click/hit-test geometry
    // (and panel-rust's own internal buffer size, via ensureHandle()) is
    // only ever recomputed reactively, through
    // QQuickWidget::SizeRootObjectToView resizing the content item on a
    // real QQuickWidget::resizeEvent, which then fires
    // RustPanelItem::geometryChange(). If the QQuickWidget's first real
    // resizeEvent never happens on its own (e.g. because Qt's dock
    // layout finalizes this widget's geometry via a direct geometry
    // assignment while it was still hidden, rather than a real resize
    // transition once visible/realized), that reactive chain never runs
    // once for real -- painting still looks right (it re-derives from
    // current size every frame) but the click-mapping side stays stale
    // until something else forces a genuine resize, which is exactly
    // what a manual redock does.
    //
    // Fix: synthesize that same genuine resize cycle here, once,
    // ourselves, right after this dock's first real show -- instead of
    // relying on the user to do it by hand via redock. Deferred one event
    // loop turn (QTimer::singleShot(0, ...)) so it runs after Qt's own
    // post-show layout pass has settled this dock's final geometry
    // (mirrors the same QTimer::singleShot(0, ...) deferral
    // MainWindow::readWindowSettings() already uses for its own
    // post-geometry resizeDocks() call).
    if (!m_didForceInitialResizeSync) {
        m_didForceInitialResizeSync = true;
        QTimer::singleShot(0, this, [this]() {
            auto *view = qobject_cast<QQuickWidget *>(widget());
            if (!view)
                return;
            const QSize size = view->size();
            if (!size.isValid() || size.isEmpty())
                return;
            // Round-trip through a genuinely different size so Qt cannot
            // skip either resizeEvent as a same-size no-op.
            view->resize(size + QSize(1, 0));
            view->resize(size);
        });
    }
}

void ChatRustDock::applyTheme(const QString &theme)
{
    if (m_panel)
        m_panel->setTheme(theme);
}

void ChatRustDock::projectOpened(const QString &path)
{
    if (m_panel)
        m_panel->setProjectPath(path);
}

void ChatRustDock::projectCreatedUntitled()
{
    if (m_panel)
        m_panel->projectCreatedUntitled();
}

void ChatRustDock::projectClosed()
{
    if (m_panel)
        m_panel->projectClosed();
}

void ChatRustDock::setProjectPath(const QString &path)
{
    if (m_panel)
        m_panel->setProjectPath(path);
}

void ChatRustDock::renameProjectPath(const QString &oldPath, const QString &newPath)
{
    if (m_panel)
        m_panel->renameProjectPath(oldPath, newPath);
}

void ChatRustDock::invokePanelCommand(int command)
{
    if (!isVisible() || !m_panel)
        return;
    m_panel->invokeCommand(static_cast<RustPanelItem::Command>(command));
}
