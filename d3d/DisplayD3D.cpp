/* Direct3D display for the mGBA Qt frontend.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "DisplayD3D.h"

#ifdef BUILD_D3D

#include "CoreController.h"
#include "LogController.h"
#include "utils.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QWaitCondition>
#include <QWindow>

#include <algorithm>
#include <cstring>
#include <vector>

#include <mgba/core/core.h>
#include <mgba/core/sync.h>
#include <mgba/core/thread.h>

#include "MessagePainter.h"

using namespace QGBA;

static_assert(sizeof(color_t) == 4, "The Direct3D display needs 32-bit video output");

namespace {

qreal pixelRatio(const QWidget* widget) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 6, 0))
	return widget->devicePixelRatioF();
#else
	return widget->devicePixelRatio();
#endif
}

}

// State shared between the GUI thread and the render thread, plus the render
// thread itself. Everything under "shared" is guarded by mutex.
struct DisplayD3D::Impl : public QThread {
	explicit Impl(D3DRenderer::Backend b)
		: backend(b)
	{
		setObjectName(QStringLiteral("Direct3D Painter Thread"));
	}

	void run() override;

	const D3DRenderer::Backend backend;

	QMutex mutex;
	QWaitCondition work;
	QWaitCondition ack;

	// shared
	bool quit = false;
	bool active = false;
	bool visible = false;
	bool inSync = false;
	bool redraw = false;
	CoreController* context = nullptr;
	HWND hwnd = nullptr;
	double refreshRate = 60.0;
	bool filter = false;
	bool lockAspect = false;
	bool lockInteger = false;
	bool blend = false;
	std::vector<uint32_t> pending;
	unsigned pendingW = 0;
	unsigned pendingH = 0;
	uint64_t frameSeq = 0;
	bool hasFrame = false;
	QImage overlay;
	bool overlayPresent = false;
	uint64_t overlaySeq = 0;
};

void DisplayD3D::Impl::run() {
	std::unique_ptr<D3DRenderer> renderer;
	HWND boundHwnd = nullptr;
	HWND seenHwnd = nullptr;
	HWND brokenHwnd = nullptr;
	unsigned boundW = 0;
	unsigned boundH = 0;
	bool broken = false;
	bool logged = false;
	QElapsedTimer brokenTimer;
	QElapsedTimer pace;
	pace.start();

	std::vector<uint32_t> local;
	unsigned localW = 0;
	unsigned localH = 0;
	bool localValid = false;
	bool frameOnGpu = false;
	QImage overlayLocal;
	bool overlayLocalPresent = false;
	bool overlayOnGpu = false;
	uint64_t seenFrame = 0;
	uint64_t seenOverlay = 0;
	bool paced = true;

	for (;;) {
		bool newFrame = false;
		CoreController* ctx = nullptr;
		{
			QMutexLocker lock(&mutex);
			bool timedOut = false;
			while (!quit && !timedOut) {
				if (frameSeq != seenFrame || overlaySeq != seenOverlay || redraw || hwnd != seenHwnd) {
					break;
				}
				timedOut = !work.wait(&mutex, 200);
			}
			if (quit) {
				break;
			}
			newFrame = frameSeq != seenFrame;
			if (newFrame && active && context) {
				ctx = context;
				inSync = true;
			}
		}

		// Tell the emulator thread we have seen its frame. With audio or video
		// sync enabled this is what lets it carry on with the next one.
		if (ctx) {
			mCoreThread* thread = ctx->thread();
			if (thread && thread->impl) {
				mCoreSync* sync = &thread->impl->sync;
				mCoreSyncWaitFrameStart(sync);
				paced = sync->audioWait || sync->videoFrameWait;
				mCoreSyncWaitFrameEnd(sync);
			}
			QMutexLocker lock(&mutex);
			inSync = false;
			ack.wakeAll();
		}

		double refresh;
		bool wantRedraw;
		{
			QMutexLocker lock(&mutex);
			refresh = refreshRate;
			wantRedraw = redraw;
		}

		// While fast-forwarding there is no point in drawing more often than
		// the monitor can show; leave the frame pending until it is due.
		if (newFrame && !paced && !wantRedraw) {
			qint64 interval = static_cast<qint64>(1000000000.0 / std::min(500.0, std::max(24.0, refresh)));
			qint64 elapsed = pace.nsecsElapsed();
			if (elapsed + 500000 < interval) {
				QMutexLocker lock(&mutex);
				if (!quit) {
					work.wait(&mutex, static_cast<unsigned long>((interval - elapsed + 999999) / 1000000));
				}
				continue;
			}
		}

		// Never let more than one frame pile up in front of the screen, then
		// pick up the newest emulator frame only after that wait is over.
		if (renderer) {
			renderer->waitForPresent(100);
		}

		bool drawVisible;
		bool drawFilter;
		bool drawAspect;
		bool drawInteger;
		bool drawBlend;
		HWND target;
		bool overlayChanged = false;
		{
			QMutexLocker lock(&mutex);
			if (frameSeq != seenFrame) {
				std::swap(pending, local);
				localW = pendingW;
				localH = pendingH;
				localValid = hasFrame;
				seenFrame = frameSeq;
				frameOnGpu = false;
			}
			if (overlaySeq != seenOverlay) {
				overlayLocal = overlay;
				overlayLocalPresent = overlayPresent;
				seenOverlay = overlaySeq;
				overlayOnGpu = false;
				overlayChanged = true;
			}
			redraw = false;
			drawVisible = visible;
			drawFilter = filter;
			drawAspect = lockAspect;
			drawInteger = lockInteger;
			drawBlend = blend;
			target = hwnd;
		}
		bool hwndChanged = target != seenHwnd;
		seenHwnd = target;

		if (!drawVisible || !target) {
			continue;
		}

		// (Re)build the renderer when the window handle changed or after an error
		if (renderer && boundHwnd != target) {
			renderer->shutdown();
			renderer.reset();
			boundHwnd = nullptr;
			broken = false;
		}
		if (!renderer) {
			bool retry = !broken || brokenHwnd != target || brokenTimer.elapsed() > 1000;
			RECT rc = {};
			GetClientRect(target, &rc);
			unsigned w = rc.right > rc.left ? rc.right - rc.left : 0;
			unsigned h = rc.bottom > rc.top ? rc.bottom - rc.top : 0;
			if (!retry || !w || !h) {
				continue;
			}
			std::unique_ptr<D3DRenderer> fresh = D3DRenderer::create(backend);
			if (fresh->init(target, w, h)) {
				renderer = std::move(fresh);
				boundHwnd = target;
				boundW = w;
				boundH = h;
				broken = false;
				logged = false;
				frameOnGpu = false;
				overlayOnGpu = false;
				LOG(QT, INFO) << QString::fromLatin1("%1 display ready").arg(QString::fromLatin1(renderer->name()));
			} else {
				if (!logged) {
					LOG(QT, ERROR) << QString::fromLatin1("%1 display failed to start: %2")
					                      .arg(QString::fromLatin1(fresh->name()), QString::fromStdString(fresh->lastError()));
					logged = true;
				}
				broken = true;
				brokenHwnd = target;
				brokenTimer.restart();
				continue;
			}
		}

		RECT rc = {};
		GetClientRect(boundHwnd, &rc);
		unsigned w = rc.right > rc.left ? rc.right - rc.left : 0;
		unsigned h = rc.bottom > rc.top ? rc.bottom - rc.top : 0;
		if (!w || !h) {
			continue; // minimized
		}
		bool sizeChanged = w != boundW || h != boundH;
		bool frameStale = localValid && !frameOnGpu;
		if (!(newFrame || overlayChanged || wantRedraw || hwndChanged || sizeChanged || frameStale || !overlayOnGpu)) {
			continue;
		}

		bool ok = true;
		if (sizeChanged) {
			ok = renderer->resize(w, h);
			boundW = w;
			boundH = h;
			frameOnGpu = false;
			overlayOnGpu = false;
		}
		if (ok && localValid && !frameOnGpu) {
			ok = renderer->uploadFrame(local.data(), localW, localH, localW * 4);
			frameOnGpu = ok;
		}
		if (ok && !overlayOnGpu) {
			if (overlayLocalPresent && !overlayLocal.isNull()) {
				ok = renderer->uploadOverlay(reinterpret_cast<const uint32_t*>(overlayLocal.constBits()), overlayLocal.width(),
				                             overlayLocal.height(), overlayLocal.bytesPerLine());
			} else {
				renderer->clearOverlay();
			}
			overlayOnGpu = ok;
		}
		if (ok) {
			D3DRenderer::Rect dest = { 0, 0, 0, 0 };
			if (localValid && localW && localH) {
				QRect r = clampSize(QSize(localW, localH), QSize(w, h), drawAspect, drawInteger);
				dest = { r.x(), r.y(), r.width(), r.height() };
			}
			ok = renderer->draw(dest, drawFilter, drawBlend);
		}
		if (ok) {
			ok = renderer->present(paced);
		}
		pace.restart();
		if (!ok) {
			if (!logged) {
				LOG(QT, ERROR) << QString::fromLatin1("%1 display error: %2")
				                      .arg(QString::fromLatin1(renderer->name()), QString::fromStdString(renderer->lastError()));
				logged = true;
			}
			renderer->shutdown();
			renderer.reset();
			boundHwnd = nullptr;
			broken = true;
			brokenHwnd = target;
			brokenTimer.restart();
		}
	}

	if (renderer) {
		renderer->shutdown();
	}
}

bool DisplayD3D::supports(D3DRenderer::Backend backend) {
	std::string error;
	if (D3DRenderer::probe(backend, &error)) {
		return true;
	}
	LOG(QT, WARN) << QString::fromLatin1("%1 is not available on this system: %2")
	                     .arg(QString::fromLatin1(backend == D3DRenderer::Backend::D3D11 ? "Direct3D 11" : "Direct3D 9"),
	                          QString::fromStdString(error));
	return false;
}

DisplayD3D::DisplayD3D(D3DRenderer::Backend backend, QWidget* parent)
	: Display(parent)
	, m_impl(new Impl(backend))
{
	// The swap chain needs a real window handle of its own, and Qt must not
	// paint over it.
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);

	m_osdTimer.setInterval(250);
	connect(&m_osdTimer, &QTimer::timeout, this, &DisplayD3D::forceDraw);

	m_impl->start(QThread::HighPriority);
}

DisplayD3D::~DisplayD3D() {
	stopDrawing();
	{
		QMutexLocker lock(&m_impl->mutex);
		m_impl->quit = true;
	}
	m_impl->work.wakeAll();
	m_impl->wait();
}

void DisplayD3D::startDrawing(std::shared_ptr<CoreController> controller) {
	if (m_isDrawing) {
		return;
	}
	m_isDrawing = true;
	m_started = true;
	m_context = controller;
	{
		QMutexLocker lock(&m_impl->mutex);
		m_impl->context = controller.get();
		m_impl->active = true;
		m_impl->filter = isFiltered();
		m_impl->lockAspect = isAspectRatioLocked();
		m_impl->lockInteger = isIntegerScalingLocked();
		m_impl->blend = hasInterframeBlending();
	}
	messagePainter()->resize(size(), pixelRatio(this));
	setUpdatesEnabled(false);
	if (isVisible()) {
		syncWindow();
	}
	emit drawingStarted();
}

void DisplayD3D::stopDrawing() {
	if (m_started || m_isDrawing) {
		m_isDrawing = false;
		m_started = false;
		m_osdTimer.stop();
		CoreController::Interrupter interrupter(m_context);
		{
			QMutexLocker lock(&m_impl->mutex);
			m_impl->active = false;
			m_impl->context = nullptr;
			m_impl->hasFrame = false;
			m_impl->overlayPresent = false;
			m_impl->overlay = QImage();
			++m_impl->frameSeq;
			++m_impl->overlaySeq;
			m_impl->redraw = true;
			// The render thread must be done with the core before it goes away
			while (m_impl->inSync) {
				m_impl->ack.wait(&m_impl->mutex);
			}
		}
		m_overlayShown = false;
		m_impl->work.wakeAll();
		setUpdatesEnabled(true);
	}
	m_context.reset();
}

void DisplayD3D::pauseDrawing() {
	if (m_started) {
		m_isDrawing = false;
		setUpdatesEnabled(true);
		updateOverlay();
	}
}

void DisplayD3D::unpauseDrawing() {
	if (m_started) {
		m_isDrawing = true;
		m_osdTimer.stop();
		setUpdatesEnabled(false);
	}
}

void DisplayD3D::forceDraw() {
	updateOverlay();
	requestRedraw();
}

void DisplayD3D::lockAspectRatio(bool lock) {
	Display::lockAspectRatio(lock);
	{
		QMutexLocker locker(&m_impl->mutex);
		m_impl->lockAspect = lock;
	}
	requestRedraw();
}

void DisplayD3D::lockIntegerScaling(bool lock) {
	Display::lockIntegerScaling(lock);
	{
		QMutexLocker locker(&m_impl->mutex);
		m_impl->lockInteger = lock;
	}
	requestRedraw();
}

void DisplayD3D::interframeBlending(bool enable) {
	Display::interframeBlending(enable);
	{
		QMutexLocker locker(&m_impl->mutex);
		m_impl->blend = enable;
	}
	requestRedraw();
}

void DisplayD3D::filter(bool filter) {
	Display::filter(filter);
	{
		QMutexLocker locker(&m_impl->mutex);
		m_impl->filter = filter;
	}
	requestRedraw();
}

void DisplayD3D::framePosted() {
	if (!m_context) {
		return;
	}
	const color_t* buffer = m_context->drawContext();
	if (!buffer) {
		return;
	}
	QSize dims = m_context->screenDimensions();
	if (dims.isEmpty()) {
		return;
	}
	if (isVisible()) {
		syncWindow();
	}
	updateOverlay();

	const size_t count = static_cast<size_t>(dims.width()) * dims.height();
	{
		QMutexLocker lock(&m_impl->mutex);
		if (m_impl->pending.size() < count) {
			m_impl->pending.resize(count);
		}
		memcpy(m_impl->pending.data(), buffer, count * sizeof(color_t));
		m_impl->pendingW = dims.width();
		m_impl->pendingH = dims.height();
		m_impl->hasFrame = true;
		++m_impl->frameSeq;
	}
	m_impl->work.wakeOne();
}

void DisplayD3D::resizeContext() {
	requestRedraw();
}

void DisplayD3D::paintEvent(QPaintEvent*) {
	requestRedraw();
}

void DisplayD3D::resizeEvent(QResizeEvent* event) {
	Display::resizeEvent(event);
	if (isVisible()) {
		syncWindow();
	}
	updateOverlay();
	requestRedraw();
}

void DisplayD3D::showEvent(QShowEvent* event) {
	Display::showEvent(event);
	{
		QMutexLocker lock(&m_impl->mutex);
		m_impl->visible = true;
	}
	syncWindow();
	requestRedraw();
}

void DisplayD3D::hideEvent(QHideEvent* event) {
	Display::hideEvent(event);
	QMutexLocker lock(&m_impl->mutex);
	m_impl->visible = false;
}

bool DisplayD3D::event(QEvent* event) {
	bool handled = Display::event(event);
	if (event->type() == QEvent::WinIdChange) {
		// Qt recreates the native window when the widget is reparented, which
		// invalidates the swap chain.
		syncWindow();
	}
	return handled;
}

void DisplayD3D::syncWindow() {
	HWND hwnd = reinterpret_cast<HWND>(winId());
	double refresh = 60.0;
	QWindow* handle = window() ? window()->windowHandle() : nullptr;
	if (handle && handle->screen() && handle->screen()->refreshRate() > 1.0) {
		refresh = handle->screen()->refreshRate();
	}
	bool changed = false;
	{
		QMutexLocker lock(&m_impl->mutex);
		if (m_impl->hwnd != hwnd) {
			m_impl->hwnd = hwnd;
			m_impl->redraw = true;
			changed = true;
		}
		m_impl->refreshRate = refresh;
	}
	if (changed) {
		m_impl->work.wakeAll();
	}
}

void DisplayD3D::requestRedraw() {
	{
		QMutexLocker lock(&m_impl->mutex);
		m_impl->redraw = true;
	}
	m_impl->work.wakeAll();
}

// OSD messages and the frame counter are drawn by Qt into an image that the
// render thread blends on top of the picture. This runs on the GUI thread, which
// is the thread MessagePainter lives on.
void DisplayD3D::updateOverlay() {
	const bool want = (isShowOSD() || isShowFrameCounter()) && messagePainter()->hasContent();
	if (!want) {
		if (m_overlayShown) {
			m_overlayShown = false;
			m_osdTimer.stop();
			{
				QMutexLocker lock(&m_impl->mutex);
				m_impl->overlayPresent = false;
				m_impl->overlay = QImage();
				++m_impl->overlaySeq;
			}
			m_impl->work.wakeAll();
		}
		return;
	}

	const qreal ratio = pixelRatio(this);
	QSize pixels(std::max(1, qRound(width() * ratio)), std::max(1, qRound(height() * ratio)));
	QImage image(pixels, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(ratio);
	{
		QPainter painter(&image);
		messagePainter()->paint(&painter);
	}
	{
		QMutexLocker lock(&m_impl->mutex);
		m_impl->overlay = image;
		m_impl->overlayPresent = true;
		++m_impl->overlaySeq;
	}
	m_overlayShown = true;
	m_impl->work.wakeAll();

	// While paused no frames arrive to refresh the overlay (messages expire)
	if (m_isDrawing) {
		m_osdTimer.stop();
	} else if (!m_osdTimer.isActive()) {
		m_osdTimer.start();
	}
}

#endif
