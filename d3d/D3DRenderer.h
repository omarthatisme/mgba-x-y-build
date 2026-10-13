/* Direct3D renderers for the mGBA Qt frontend.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#ifdef BUILD_D3D

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace QGBA {

// Qt-free abstraction over the two Direct3D back ends. Every method except
// probe() must be called from a single thread (the display's render thread).
class D3DRenderer {
public:
	enum class Backend {
		D3D11,
		D3D9,
	};

	struct Rect {
		int x;
		int y;
		int w;
		int h;
	};

	virtual ~D3DRenderer() = default;

	static std::unique_ptr<D3DRenderer> create(Backend);

	// Checks that the back end is usable on this machine by building a complete
	// device + swap chain on a throw-away window. The result is cached.
	static bool probe(Backend, std::string* error = nullptr);

	virtual const char* name() const = 0;

	// Creates the device and a swap chain for hwnd. width/height are the
	// client size in pixels.
	virtual bool init(HWND hwnd, unsigned width, unsigned height) = 0;
	virtual void shutdown() = 0;

	// Resizes the swap chain. Afterwards no frame is valid, so callers must
	// upload one again before draw().
	virtual bool resize(unsigned width, unsigned height) = 0;

	// pixels are mGBA's color_t: bytes R, G, B, X. pitch is in bytes. Each call
	// makes the new frame current and keeps the previous one for interframe
	// blending.
	virtual bool uploadFrame(const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) = 0;

	// Premultiplied RGBA bytes covering the whole client area (OSD messages).
	virtual bool uploadOverlay(const uint32_t* pixels, unsigned width, unsigned height, unsigned pitch) = 0;
	virtual void clearOverlay() = 0;

	// Blocks (up to timeoutMs) until the swap chain can take another frame
	// without increasing latency.
	virtual void waitForPresent(unsigned timeoutMs) = 0;

	// Clears the back buffer to black and draws the current frame into dest
	// (client pixels), then the overlay.
	virtual bool draw(const Rect& dest, bool filter, bool blend) = 0;

	// Returns false if the device was lost and the renderer has to be rebuilt.
	virtual bool present(bool vsync) = 0;

	// Reads the back buffer (after draw(), before present()) as RGBA bytes.
	virtual bool capture(std::vector<uint32_t>& rgba, unsigned& width, unsigned& height) = 0;

	const std::string& lastError() const { return m_error; }

protected:
	std::string m_error;
};

}

#endif
