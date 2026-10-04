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

#ifndef CRYSTALDOCK_WINDOW_THUMBNAIL_H_
#define CRYSTALDOCK_WINDOW_THUMBNAIL_H_

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <model/multi_dock_model.h>  // PanelPosition

#include <display/kde_window_manager.h>
#include <display/window_system.h>

namespace crystaldock {

// Live window thumbnails for Plasma/KWin (Wayland), implemented on top of
// KF6 WindowSystem (KWindowInfo) + KWin's D-Bus thumbnail interfaces.
//
// Wayland note: unlike X11, a client cannot grab another client's surface
// contents directly (there is no XGetImage/XComposite equivalent), which is
// why plain Qt/KWindowSystem::thumbnail() does nothing under Wayland. On
// Plasma 6 the compositor cooperates instead: plasma-wayland-protocols'
// org_kde_plasma_window gives every managed window an internal UUID, and KWin
// exposes grabs keyed by that UUID over D-Bus (org.kde.KWin /Thumbnailer with
// org.kde.KWin /Compositor as fallback) -- exactly the mechanism KDE's own
// task bar uses for hover previews. We therefore:
//   - resolve window -> uuid via KdeWindowManager (plasma-window-management),
//   - fetch frames asynchronously from KWin via Qt6::DBus,
//   - cache them in a QCache keyed by uuid,
//   - invalidate on WindowSystem's windowChanged-equivalent signals
//     (windowRemoved / windowGeometryChanged / windowStateChanged /
//     windowTitleChanged) so we never poll the compositor needlessly.
//
// Architecture:
//   - WindowThumbnailProvider: singleton provider/cache described above.
//   - WindowThumbnailPopup: frameless Qt::Tip popup shown next to the hovered
//     dock icon after a hover delay (~350 ms) to avoid flickering when the
//     cursor just sweeps across the dock.
class WindowThumbnailProvider : public QObject {
  Q_OBJECT

 public:
  static WindowThumbnailProvider* self();

  // True if this session supports thumbnails (Plasma/Wayland + KWin bridge).
  bool available() const { return available_; }

  // Returns a cached thumbnail if available. On miss, schedules an async
  // fetch from KWin; when the reply arrives, thumbnailReady() is emitted and
  // the popup re-reads the cache. Never blocks.
  QPixmap getThumbnail(void* window);

  // The plasma-window-management uuid of a window (empty on non-KDE DEs).
  QString windowUuid(void* window);

  // Drop the cached thumbnail for a window (e.g. it was closed or unmapped).
  void invalidate(void* window);

 public slots:
  // Drop everything (e.g. KWin restarted / lost on the bus).
  void clearAll();

 signals:
  void thumbnailReady(const QString& uuid);

 private:
  WindowThumbnailProvider();
  Q_DISABLE_COPY(WindowThumbnailProvider)

  struct Entry {
    QPixmap pixmap;
    qint64 fetchedAtMs = 0;  // For rate-limiting live refreshes.
  };

  // Ask KWin's Thumbnailer service for a frame asynchronously.
  void requestViaThumbnailer(const QString& uuid);
  // Fallback: ask the Compositor interface directly.
  void requestViaCompositor(const QString& uuid);
  void handleThumbnailReply(const QString& uuid, const QImage& image);

  // Slots connected to WindowSystem signals for cache invalidation.
  void onWindowRemoved(void* window);
  void onWindowGeometryChanged(const WindowInfo* info);
  void onWindowStateChanged(const WindowInfo* info);
  void onWindowTitleChanged(const WindowInfo* info);

  static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

  bool available_ = false;
  QCache<QString, Entry> cache_{ 8 };      // At most 8 thumbnails (~2 MB).
  QSet<QString> pending_;                  // In-flight D-Bus calls.
  QHash<QString, qint64> lastRequestMs_;   // Rate limiting per window.

  // A full-size fetch costs one compositor round-trip; only refetch "live"
  // (i.e. while the pointer keeps hovering) at most every this many ms.
  static constexpr qint64 kMinRefreshIntervalMs = 1000;
  static constexpr int kMaxThumbnailWidth = 480;
  static constexpr int kMaxThumbnailHeight = 270;

  friend class WindowThumbnailPopup;
};

// The popup widget that displays the thumbnail of the hovered window.
class WindowThumbnailPopup : public QWidget {
  Q_OBJECT

 public:
  explicit WindowThumbnailPopup(QWidget* parent = nullptr);

  // Called by Program when the hover timer fires. `itemRect` is in global
  // coordinates; `position` is the dock edge so we know which side to pop out.
  void showForWindow(void* window, const QRect& itemRect, PanelPosition position,
                     const QString& title);

  // Grace-period hide so moving between the icon and the popup doesn't flicker.
  void hideWithDelay();
  void cancelHide();

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  void updatePixmapAndSize();
  QPoint computePos(const QRect& itemRect, PanelPosition position) const;

  QTimer hideTimer_;
  void* window_ = nullptr;
  QPixmap pixmap_;
  QString title_;
  QFont titleFont_;

  static constexpr int kPadding = 8;
  static constexpr int kTitleHeight = 22;
  static constexpr int kCornerRadius = 8;
  static constexpr int kHideDelayMs = 250;  // Grace period when leaving the icon.

  static constexpr int kFallbackWidth = 320;
  static constexpr int kFallbackHeight = 180;
};

}  // namespace crystaldock

#endif  // CRYSTALDOCK_WINDOW_THUMBNAIL_H_
