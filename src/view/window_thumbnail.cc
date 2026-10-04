/*
 * This file is part of Crystal Dock.
 * Copyright (C) 2026 Crystal Dock contributors
 *
 * Crystal Dock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Crystal Dock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Crystal Dock.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "window_thumbnail.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>

// KF6 WindowSystem (KF6::WindowSystem). Used to detect the platform session.
// The actual pixel grab goes through KWin's D-Bus interfaces below, because on
// Wayland only the compositor may access other clients' buffers (this is the
// plasma-wayland-protocols / org.kde.KWin workaround;
// KWindowSystem::thumbnail() is X11-only and returns null on Wayland).
#include <KWindowSystem>

namespace crystaldock {

namespace {

// KWin's thumbnail service introduced with Plasma 6.x scripting API.
const char* kThumbnailerService = "org.kde.KWin";
const char* kThumbnailerPath = "/Thumbnailer";
const char* kThumbnailerIface = "org.kde.KWin.Thumbnailer";

// Older/alternative path used by KDE's task bar implementations.
const char* kCompositorService = "org.kde.KWin";
const char* kCompositorPath = "/Compositor";
const char* kCompositorIface = "org.kde.KWin.Compositor";

}  // namespace

/* static */ WindowThumbnailProvider* WindowThumbnailProvider::self() {
  static WindowThumbnailProvider instance;
  return &instance;
}

WindowThumbnailProvider::WindowThumbnailProvider() {
  // Thumbnails require KWin's plasma-window-management bridge (KDE/Plasma
  // Wayland session). On other DEs (Hyprland/Sway/labwc/niri/Wayfire...) we
  // simply disable the feature instead of falling back to broken X11 calls.
  available_ = KWindowSystem::isPlatformWayland() &&
               WindowSystem::hasKdeWindowManagement();

  // Cache invalidation driven by WindowSystem signals -- these are our dock's
  // equivalent of KWindowSystem::windowChanged(WIN_ID, ...): we never poll
  // KWin unless the user actually hovers an icon AND something changed.
  auto* ws = WindowSystem::self();
  connect(ws, &WindowSystem::windowRemoved, this,
          &WindowThumbnailProvider::onWindowRemoved);
  connect(ws, &WindowSystem::windowGeometryChanged, this,
          &WindowThumbnailProvider::onWindowGeometryChanged);
  connect(ws, &WindowSystem::windowStateChanged, this,
          &WindowThumbnailProvider::onWindowStateChanged);
  connect(ws, &WindowSystem::windowTitleChanged, this,
          &WindowThumbnailProvider::onWindowTitleChanged);

  // If KWin goes away (restart / crash), all cached buffers are stale.
  QDBusConnection::sessionBus().connect(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/Thumbnailer"),
      QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"),
      this, SLOT(clearAll()));
}

QString WindowThumbnailProvider::windowUuid(void* window) {
  if (!available_) {
    return QString();
  }
  // KdeWindowManager keeps the plasma-window-management uuid for every managed
  // window; KWin's thumbnail services key their grabs by that same uuid.
  return KdeWindowManager::self()->uuidOfWindow(window);
}

QPixmap WindowThumbnailProvider::getThumbnail(void* window) {
  if (!available_ || window == nullptr) {
    return QPixmap();
  }
  const QString uuid = windowUuid(window);
  if (uuid.isEmpty()) {
    return QPixmap();
  }

  if (cache_.contains(uuid)) {
    const Entry* entry = cache_.object(uuid);
    // Cached hit: show the stale frame immediately (no blank popup / flicker)
    // and refresh in the background if it got old while the pointer stayed.
    if (nowMs() - entry->fetchedAtMs > kMinRefreshIntervalMs &&
        !pending_.contains(uuid)) {
      requestViaThumbnailer(uuid);
    }
    return entry->pixmap;
  }

  if (!pending_.contains(uuid)) {
    requestViaThumbnailer(uuid);
  }
  return QPixmap();
}

void WindowThumbnailProvider::invalidate(void* window) {
  const QString uuid = windowUuid(window);
  if (!uuid.isEmpty()) {
    cache_.remove(uuid);
    lastRequestMs_.remove(uuid);
  }
}

void WindowThumbnailProvider::clearAll() {
  cache_.clear();
  pending_.clear();
  lastRequestMs_.clear();
}

void WindowThumbnailProvider::requestViaThumbnailer(const QString& uuid) {
  // Rate-limit per window: never ask the compositor more than once per
  // kMinRefreshIntervalMs, regardless of how many hover events arrive.
  const qint64 last = lastRequestMs_.value(uuid, 0);
  if (nowMs() - last < kMinRefreshIntervalMs) {
    return;
  }
  lastRequestMs_[uuid] = nowMs();
  pending_.insert(uuid);

  QDBusInterface iface(QLatin1String(kThumbnailerService),
                       QLatin1String(kThumbnailerPath),
                       QLatin1String(kThumbnailerIface),
                       QDBusConnection::sessionBus());
  if (!iface.isValid()) {  // Interface not exported by this KWin build.
    requestViaCompositor(uuid);
    return;
  }

  // org.kde.KWin.Thumbnailer::requestThumbnail(QString uuid, int width,
  //                                            int height) -> QImage
  QDBusPendingCall call = iface.asyncCall(QStringLiteral("requestThumbnail"),
                                          uuid, kMaxThumbnailWidth,
                                          kMaxThumbnailHeight);
  auto* watcher = new QDBusPendingCallWatcher(call, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, uuid](QDBusPendingCallWatcher* w) {
            QImage image;
            const QDBusReply<QImage> reply = *w;
            if (reply.isValid()) {
              image = reply.value();
            }
            handleThumbnailReply(uuid, image);
            w->deleteLater();
          });
}

void WindowThumbnailProvider::requestViaCompositor(const QString& uuid) {
  QDBusInterface iface(QLatin1String(kCompositorService),
                       QLatin1String(kCompositorPath),
                       QLatin1String(kCompositorIface),
                       QDBusConnection::sessionBus());
  if (!iface.isValid()) {
    pending_.remove(uuid);
    return;
  }

  // org.kde.KWin.Compositor::currentWindowThumbnail(QString uuid) -> QImage
  // Same mechanism KDE's own task bar uses for hover previews on Wayland.
  QDBusPendingCall call = iface.asyncCall(
      QStringLiteral("currentWindowThumbnail"), uuid);
  auto* watcher = new QDBusPendingCallWatcher(call, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, uuid](QDBusPendingCallWatcher* w) {
            QImage image;
            const QDBusReply<QImage> reply = *w;
            if (reply.isValid()) {
              image = reply.value();
            }
            handleThumbnailReply(uuid, image);
            w->deleteLater();
          });
}

void WindowThumbnailProvider::handleThumbnailReply(const QString& uuid,
                                                   const QImage& image) {
  pending_.remove(uuid);
  if (image.isNull()) {
    // Failed grab (e.g. window is minimized/XDG-hidden). Keep whatever we had
    // cached; lastRequestMs_ prevents immediate retries.
    return;
  }

  // KWin returns buffer-resolution images; tag them with the screen DPR so the
  // popup renders at physical detail but logical size (HiDPI-friendly, cf.
  // the blurry-icon fix in commit history).
  qreal dpr = 1.0;
  if (auto* screen = QGuiApplication::primaryScreen()) {
    dpr = screen->devicePixelRatio();
  }
  QPixmap pixmap = QPixmap::fromImage(image);
  pixmap.setDevicePixelRatio(dpr);

  cache_.insert(uuid, new Entry{pixmap, nowMs()});  // QCache owns the entry.
  emit thumbnailReady(uuid);
}

void WindowThumbnailProvider::onWindowRemoved(void* window) {
  invalidate(window);
}

void WindowThumbnailProvider::onWindowGeometryChanged(const WindowInfo* info) {
  // Size changes invalidate the aspect ratio of the cached frame.
  if (info) {
    invalidate(info->window);
  }
}

void WindowThumbnailProvider::onWindowStateChanged(const WindowInfo* info) {
  if (info && (info->minimized || info->fullscreen)) {
    invalidate(info->window);
  }
}

void WindowThumbnailProvider::onWindowTitleChanged(const WindowInfo* info) {
  // Cheap heuristic: title churn usually means navigation; drop the frame so
  // the next hover re-grabs fresh content.
  if (info) {
    invalidate(info->window);
  }
}

// -----------------------------------------------------------------------------
// WindowThumbnailPopup
// -----------------------------------------------------------------------------

WindowThumbnailPopup::WindowThumbnailPopup(QWidget* parent)
    : QWidget(parent, Qt::TipWindow | Qt::FramelessWindowHint |
                        Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint) {
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TranslucentBackground);
  setFocusPolicy(Qt::NoFocus);
  hide();

  titleFont_ = font();
  titleFont_.setPointSizeF(titleFont_.pointSizeF() * 0.9);
  titleFont_.setBold(true);

  hideTimer_.setSingleShot(true);
  hideTimer_.setInterval(kHideDelayMs);
  connect(&hideTimer_, &QTimer::timeout, this, &QWidget::hide);

  // Repaint as soon as a fresh thumbnail arrives for the window we display.
  connect(WindowThumbnailProvider::self(), &WindowThumbnailProvider::thumbnailReady,
          this, [this](const QString&) {
            if (isVisible() && window_) {
              updatePixmapAndSize();
            }
          });
}

void WindowThumbnailPopup::showForWindow(void* window, const QRect& itemRect,
                                         PanelPosition position,
                                         const QString& title) {
  window_ = window;
  title_ = title;
  cancelHide();
  updatePixmapAndSize();
  move(computePos(itemRect, position));
  show();
  raise();
}

void WindowThumbnailPopup::updatePixmapAndSize() {
  pixmap_ = WindowThumbnailProvider::self()->getThumbnail(window_);

  int w = kFallbackWidth;
  int h = kFallbackHeight;
  if (!pixmap_.isNull()) {
    const qreal dpr = pixmap_.devicePixelRatio();
    w = qRound(pixmap_.width() / dpr);
    h = qRound(pixmap_.height() / dpr);
  }
  resize(w + 2 * kPadding, h + 2 * kPadding + (title_.isEmpty() ? 0 : kTitleHeight));
  update();
}

QPoint WindowThumbnailPopup::computePos(const QRect& itemRect,
                                        PanelPosition position) const {
  QRect available;
  if (auto* screen = QGuiApplication::screenAt(itemRect.center())) {
    available = screen->availableGeometry();
  } else if (auto* screen = QGuiApplication::primaryScreen()) {
    available = screen->availableGeometry();
  } else {
    return itemRect.topLeft();
  }

  int x = itemRect.center().x() - width() / 2;
  int y;
  switch (position) {
    case PanelPosition::Top:
      y = itemRect.bottom() + kPadding;
      break;
    case PanelPosition::Bottom:
    default:
      y = itemRect.top() - height() - kPadding;
      break;
    case PanelPosition::Left:
      x = itemRect.right() + kPadding;
      y = itemRect.center().y() - height() / 2;
      break;
    case PanelPosition::Right:
      x = itemRect.left() - width() - kPadding;
      y = itemRect.center().y() - height() / 2;
      break;
  }

  // Clamp to the screen so the popup never gets pushed off-view.
  x = qBound(available.left(), x, available.right() - width());
  y = qBound(available.top(), y, available.bottom() - height());
  return QPoint(x, y);
}

void WindowThumbnailPopup::hideWithDelay() {
  hideTimer_.start();
}

void WindowThumbnailPopup::cancelHide() {
  hideTimer_.stop();
}

void WindowThumbnailPopup::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  const QRectF rect(0, 0, width(), height());
  QPainterPath path;
  path.addRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), kCornerRadius,
                      kCornerRadius);
  painter.fillPath(path, QColor(35, 35, 40, 230));
  painter.strokePath(path, QPen(QColor(255, 255, 255, 60), 1));

  const int imgH = height() - 2 * kPadding - (title_.isEmpty() ? 0 : kTitleHeight);
  const QRectF target(kPadding, kPadding, width() - 2 * kPadding, imgH);

  if (pixmap_.isNull()) {
    // Placeholder while the async grab is in flight.
    painter.setPen(QColor(200, 200, 200, 120));
    painter.drawText(target, Qt::AlignCenter, QStringLiteral("…"));
  } else {
    QRectF centered(0, 0, target.width(),
                    target.width() * pixmap_.height() / pixmap_.width());
    centered.moveCenter(target.center());
    painter.drawPixmap(centered, pixmap_,
                       QRectF(QPointF(0, 0), pixmap_.size()));
  }

  if (!title_.isEmpty()) {
    painter.setFont(titleFont_);
    painter.setPen(QColor(235, 235, 235));
    const QRectF titleRect(kPadding, height() - kPadding - kTitleHeight,
                           width() - 2 * kPadding, kTitleHeight);
    painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter,
                     QFontMetrics(titleFont_).elidedText(title_, Qt::ElideMiddle,
                                                         int(titleRect.width())));
  }
}

}  // namespace crystaldock
