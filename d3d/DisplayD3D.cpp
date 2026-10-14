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

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QScreen>
#include <QTextStream>
#include <QThread>
#include <QWaitCondition>
#include <QWindow>

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
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
		, stats(qEnvironmentVariableIsSet("MGBA_D3D_STATS"))
	{
		setObjectName(QStringLiteral("Direct3D Painter Thread"));
		clock.start();
		bool ok = false;
		int depth = qEnvironmentVariableIntValue("MGBA_D3D_QUEUE", &ok);
		if (ok) {
			queueDepth = static_cast<unsigned>(std::min(4, std::max(1, depth)));
			queueLimit = queueDepth;
		}
	}

	struct Frame {
		std::vector<uint32_t> pixels;
		unsigned w = 0;
		unsigned h = 0;
	};

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
	// Frames waiting to be shown, oldest first. The emulator can deliver
	// frames in bursts (an audio device that wakes it every ~30 ms gives pairs
	// of frames a few ms apart), so a single "newest" slot would throw away
	// every other frame of a burst. A short in-order queue shows them all, one
	// per screen refresh; frames beyond queueLimit are dropped oldest-first so
	// the delay stays bounded. While fast-forwarding the limit is 1 (newest only).
	std::deque<Frame> queue;
	std::vector<Frame> spare;
	unsigned queueDepth = 2;
	unsigned queueLimit = 2;
	bool clearFrame = false;
	uint64_t frameSeq = 0;
	QImage overlay;
	bool overlayPresent = false;
	uint64_t overlaySeq = 0;

	// Diagnostics, enabled by setting the MGBA_D3D_STATS environment variable.
	// The arrival_* numbers are collected on the GUI thread, under mutex.
	const bool stats;
	QElapsedTimer clock;
	qint64 lastArrival = 0;
	qint64 arrivals = 0;
	qint64 arrivalsLate = 0;
	qint64 arrivalMax = 0;
	qint64 dropped = 0;
	double arrivalSum = 0;
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

	bool paced = true;
	QFile statsFile;
	QTextStream statsOut;
	qint64 statStart = 0;
	qint64 presents = 0;
	qint64 presentsLate = 0;
	qint64 presentGapMax = 0;
	qint64 presentBlockMax = 0;
	qint64 lastPresent = 0;
	double presentGapSum = 0;
	if (stats) {
		statsFile.setFileName(QCoreApplication::applicationDirPath() + QStringLiteral("/mgba-d3d-stats.txt"));
		if (!statsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
			statsFile.setFileName(QDir::tempPath() + QStringLiteral("/mgba-d3d-stats.txt"));
			statsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
		}
		statsOut.setDevice(&statsFile);
		statsOut << "Stats file for the mGBA Direct3D display. Each line covers about 2 seconds.\n";
		statsOut.flush();
		statStart = clock.nsecsElapsed();
	}
	auto ms = [](double ns) { return QString::number(ns / 1000000.0, 'f', 1); };
	auto flushStats = [&]() {
		qint64 now = clock.nsecsElapsed();
		qint64 aCount;
		qint64 aLate;
		qint64 aMax;
		qint64 aDropped;
		double aSum;
		double refresh;
		{
			QMutexLocker lock(&mutex);
			aCount = arrivals;
			aLate = arrivalsLate;
			aMax = arrivalMax;
			aDropped = dropped;
			aSum = arrivalSum;
			refresh = refreshRate;
			arrivals = arrivalsLate = arrivalMax = dropped = 0;
			arrivalSum = 0;
		}
		statsOut << QStringLiteral("t=%1s refresh=%2Hz paced=%3 | frames in: %4 (gap avg %5 max %6 ms, over 25 ms: %7), dropped: %8 | "
		                           "presented: %9 (gap avg %10 max %11 ms, over 25 ms: %12), present() blocked max %13 ms")
		                .arg(QString::number(now / 1e9, 'f', 1))
		                .arg(QString::number(refresh, 'f', 2))
		                .arg(paced ? 1 : 0)
		                .arg(aCount)
		                .arg(aCount > 0 ? ms(aSum / aCount) : QStringLiteral("-"))
		                .arg(ms(aMax))
		                .arg(aLate)
		                .arg(aDropped)
		                .arg(presents)
		                .arg(presents > 0 ? ms(presentGapSum / presents) : QStringLiteral("-"))
		                .arg(ms(presentGapMax))
		                .arg(presentsLate)
		                .arg(ms(presentBlockMax))
		        << '\n';
		statsOut.flush();
		statStart = now;
		presents = presentsLate = presentGapMax = presentBlockMax = 0;
		presentGapSum = 0;
	};

	Frame local;
	bool localValid = false;
	bool frameOnGpu = false;
	QImage overlayLocal;
	bool overlayLocalPresent = false;
	bool overlayOnGpu = false;
	uint64_t ackedSeq = 0;
	uint64_t seenOverlay = 0;

	for (;;) {
		bool arrived = false;
		bool queued = false;
		CoreController* ctx = nullptr;
		if (stats && clock.nsecsElapsed() - statStart >= 2000000000LL) {
			flushStats();
		}
		{
			QMutexLocker lock(&mutex);
			bool timedOut = false;
			while (!quit && !timedOut) {
				if (!queue.empty() || frameSeq != ackedSeq || overlaySeq != seenOverlay || redraw || hwnd != seenHwnd) {
					break;
				}
				timedOut = !work.wait(&mutex, 200);
			}
			if (quit) {
				break;
			}
			arrived = frameSeq != ackedSeq;
			queued = !queue.empty();
			ackedSeq = frameSeq;
			if (arrived && active && context) {
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
			queueLimit = paced ? queueDepth : 1;
			while (queue.size() > queueLimit) {
				spare.push_back(std::move(queue.front()));
				queue.pop_front();
				++dropped;
			}
		}

		// While fast-forwarding there is no point in drawing more often than
		// the monitor can show; leave the frame pending until it is due.
		if (queued && !paced && !wantRedraw) {
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
		bool gotFrame = false;
		{
			QMutexLocker lock(&mutex);
			if (clearFrame) {
				clearFrame = false;
				localValid = false;
				frameOnGpu = false;
			}
			if (!queue.empty()) {
				Frame next = std::move(queue.front());
				queue.pop_front();
				std::swap(local, next);
				spare.push_back(std::move(next));
				localValid = true;
				frameOnGpu = false;
				gotFrame = true;
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
				if (stats) {
					statsOut << "renderer: " << renderer->name() << " - " << QString::fromStdString(renderer->modeDescription()) << '\n';
					statsOut.flush();
				}
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
		if (!(gotFrame || overlayChanged || wantRedraw || hwndChanged || sizeChanged || frameStale || !overlayOnGpu)) {
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
			ok = renderer->uploadFrame(local.pixels.data(), local.w, local.h, local.w * 4);
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
			if (localValid && local.w && local.h) {
				QRect r = clampSize(QSize(local.w, local.h), QSize(w, h), drawAspect, drawInteger);
				dest = { r.x(), r.y(), r.width(), r.height() };
			}
			ok = renderer->draw(dest, drawFilter, drawBlend);
		}
		if (ok) {
			qint64 before = stats ? clock.nsecsElapsed() : 0;
			ok = renderer->present(paced);
			if (stats) {
				qint64 after = clock.nsecsElapsed();
				presentBlockMax = std::max(presentBlockMax, after - before);
				if (gotFrame && ok) {
					if (lastPresent) {
						qint64 gap = after - lastPresent;
						++presents;
						presentGapSum += gap;
						presentGapMax = std::max(presentGapMax, gap);
						if (gap > 25000000) {
							++presentsLate;
						}
					} else {
						++presents;
					}
					lastPresent = after;
				}
			}
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
			while (!m_impl->queue.empty()) {
				m_impl->spare.push_back(std::move(m_impl->queue.front()));
				m_impl->queue.pop_front();
			}
			m_impl->clearFrame = true;
			m_impl->overlayPresent = false;
			m_impl->overlay = QImage();
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
		Impl::Frame frame;
		if (!m_impl->spare.empty()) {
			frame = std::move(m_impl->spare.back());
			m_impl->spare.pop_back();
		}
		if (frame.pixels.size() < count) {
			frame.pixels.resize(count);
		}
		memcpy(frame.pixels.data(), buffer, count * sizeof(color_t));
		frame.w = dims.width();
		frame.h = dims.height();
		while (m_impl->queue.size() >= m_impl->queueLimit) {
			m_impl->spare.push_back(std::move(m_impl->queue.front()));
			m_impl->queue.pop_front();
			++m_impl->dropped; // never shown
		}
		m_impl->queue.push_back(std::move(frame));
		if (m_impl->stats) {
			qint64 now = m_impl->clock.nsecsElapsed();
			if (m_impl->lastArrival) {
				qint64 gap = now - m_impl->lastArrival;
				++m_impl->arrivals;
				m_impl->arrivalSum += gap;
				m_impl->arrivalMax = std::max(m_impl->arrivalMax, gap);
				if (gap > 25000000) {
					++m_impl->arrivalsLate;
				}
			} else {
				++m_impl->arrivals;
			}
			m_impl->lastArrival = now;
		}
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
