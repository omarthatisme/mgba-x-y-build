/* Direct3D display for the mGBA Qt frontend.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#ifdef BUILD_D3D

#include "Display.h"
#include "D3DRenderer.h"

#include <QTimer>

#include <memory>

namespace QGBA {

// A display that renders on its own thread through Direct3D 11 or Direct3D 9.
// Unlike the OpenGL display there is no queue of old frames: the render thread
// always presents the newest frame the emulator has produced, which keeps the
// delay between input and picture as short as the swap chain allows.
class DisplayD3D : public Display {
Q_OBJECT

public:
	static bool supports(D3DRenderer::Backend);

	DisplayD3D(D3DRenderer::Backend, QWidget* parent = nullptr);
	~DisplayD3D();

	void startDrawing(std::shared_ptr<CoreController>) override;
	bool isDrawing() const override { return m_isDrawing; }
	bool supportsShaders() const override { return false; }
	VideoShader* shaders() override { return nullptr; }

public slots:
	void stopDrawing() override;
	void pauseDrawing() override;
	void unpauseDrawing() override;
	void forceDraw() override;
	void lockAspectRatio(bool lock) override;
	void lockIntegerScaling(bool lock) override;
	void interframeBlending(bool enable) override;
	void filter(bool filter) override;
	void swapInterval(int) override {}
	void framePosted() override;
	void setShaders(struct VDir*) override {}
	void clearShaders() override {}
	void resizeContext() override;

protected:
	QPaintEngine* paintEngine() const override { return nullptr; }
	void paintEvent(QPaintEvent*) override;
	void resizeEvent(QResizeEvent*) override;
	void showEvent(QShowEvent*) override;
	void hideEvent(QHideEvent*) override;
	bool event(QEvent*) override;

private:
	struct Impl;

	void syncWindow();
	void updateOverlay();
	void requestRedraw();

	std::unique_ptr<Impl> m_impl;
	std::shared_ptr<CoreController> m_context;
	QTimer m_osdTimer;
	bool m_isDrawing = false;
	bool m_started = false;
	bool m_overlayShown = false;
};

}

#endif
