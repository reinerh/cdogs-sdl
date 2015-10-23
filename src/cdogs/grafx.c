/*
    C-Dogs SDL
    A port of the legendary (and fun) action/arcade cdogs.
    Copyright (C) 1995 Ronny Wester
    Copyright (C) 2003 Jeremy Chin
    Copyright (C) 2003-2007 Lucas Martin-King

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    This file incorporates work covered by the following copyright and
    permission notice:

    Copyright (c) 2013-2015, Cong Xu
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/
#include "grafx.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>

#include <SDL_events.h>
#include <SDL_image.h>
#include <SDL_mouse.h>

#include "blit.h"
#include "config.h"
#include "defs.h"
#include "grafx_bg.h"
#include "log.h"
#include "palette.h"
#include "files.h"
#include "utils.h"


typedef struct
{
	int Width;
	int Height;
	int ScaleFactor;
} GraphicsMode;


GraphicsDevice gGraphicsDevice;

static void Gfx_ModeSet(const GraphicsMode *mode)
{
	ConfigGet(&gConfig, "Graphics.ResolutionWidth")->u.Int.Value = mode->Width;
	ConfigGet(&gConfig, "Graphics.ResolutionHeight")->u.Int.Value = mode->Height;
	ConfigGet(&gConfig, "Graphics.ScaleFactor")->u.Int.Value = mode->ScaleFactor;
}

void Gfx_ModePrev(void)
{
	GraphicsDevice *device = &gGraphicsDevice;
	device->modeIndex--;
	if (device->modeIndex < 0)
	{
		device->modeIndex = (int)device->validModes.size - 1;
	}
	Gfx_ModeSet(CArrayGet(&device->validModes, device->modeIndex));
}

void Gfx_ModeNext(void)
{
	GraphicsDevice *device = &gGraphicsDevice;
	device->modeIndex++;
	if (device->modeIndex >= (int)device->validModes.size)
	{
		device->modeIndex = 0;
	}
	Gfx_ModeSet(CArrayGet(&device->validModes, device->modeIndex));
}

static int FindValidMode(
	GraphicsDevice *device, const int w, const int h, const int scale)
{
	for (int i = 0; i < (int)device->validModes.size; i++)
	{
		const GraphicsMode *mode = CArrayGet(&device->validModes, i);
		if (mode->Width == w && mode->Height == h &&
			mode->ScaleFactor == scale)
		{
			return i;
		}
	}
	return -1;
}

static void AddGraphicsMode(
	GraphicsDevice *device, const int w, const int h, const int scale)
{
	// Don't add if mode already exists
	if (FindValidMode(device, w, h, scale) != -1)
	{
		return;
	}

	const int size = w * h * scale * scale;
	int i;
	for (i = 0; i < (int)device->validModes.size; i++)
	{
		// Ordered by actual resolution ascending and scale descending
		const GraphicsMode *mode = CArrayGet(&device->validModes, i);
		const int actualResolution = mode->Width * mode->Height;
		if (actualResolution > size ||
			(actualResolution == size && mode->ScaleFactor < scale))
		{
			break;
		}
	}
	GraphicsMode mode;
	mode.Width = w;
	mode.Height = h;
	mode.ScaleFactor = scale;
	CArrayInsert(&device->validModes, i, &mode);
}

void GraphicsInit(GraphicsDevice *device, Config *c)
{
	device->IsInitialized = 0;
	device->IsWindowInitialized = 0;
	device->screen = NULL;
	device->renderer = NULL;
	device->window = NULL;
	memset(&device->cachedConfig, 0, sizeof device->cachedConfig);
	CArrayInit(&device->validModes, sizeof(GraphicsMode));
	device->modeIndex = 0;
	device->clipping.left = 0;
	device->clipping.top = 0;
	device->clipping.right = 0;
	device->clipping.bottom = 0;
	// Add default modes
	AddGraphicsMode(device, 320, 240, 1);
	AddGraphicsMode(device, 400, 300, 1);
	AddGraphicsMode(device, 640, 480, 1);
	AddGraphicsMode(device, 320, 240, 2);
	device->buf = NULL;
	device->bkg = NULL;
	GraphicsConfigSetFromConfig(&device->cachedConfig, c);
}

static void AddSupportedGraphicsModes(GraphicsDevice *device)
{
	// TODO: multiple window support?
	const int numDisplayModes = SDL_GetNumDisplayModes(0);
	if (numDisplayModes < 1)
	{
		LOG(LM_MAIN, LL_ERROR, "no valid display modes: %s\n", SDL_GetError());
		return;
	}
	for (int i = 0; i < numDisplayModes; i++)
	{
		SDL_DisplayMode mode;
		if (SDL_GetDisplayMode(0, i, &mode) != 0)
		{
			LOG(LM_MAIN, LL_ERROR, "cannot get display mode: %s\n",
				SDL_GetError());
			continue;
		}
		int validScaleFactors[] = { 1, 2, 3, 4 };
		for (int j = 0; j < 4; j++)
		{
			const int scaleFactor = validScaleFactors[j];
			if (mode.w % scaleFactor || mode.h % scaleFactor)
			{
				continue;
			}
			if (mode.w % 4)
			{
				// TODO: why does width have to be divisible by 4? 1366x768 doesn't work
				continue;
			}
			const int w = mode.w / scaleFactor;
			const int h = mode.h / scaleFactor;
			if (w < 320 || h < 240)
			{
				break;
			}
			AddGraphicsMode(device, w, h, scaleFactor);
		}
	}
}

// Initialises the video subsystem.
// To prevent needless screen flickering, config is compared with cache
// to see if anything changed. If not, don't recreate the screen.
void GraphicsInitialize(GraphicsDevice *g, const bool force)
{
	if (g->IsInitialized && !g->cachedConfig.needRestart)
	{
		return;
	}

	if (!g->IsWindowInitialized)
	{
		char buf[CDOGS_PATH_MAX];
		GetDataFilePath(buf, "cdogs_icon.bmp");
		g->icon = IMG_Load(buf);
		AddSupportedGraphicsModes(g);
		g->IsWindowInitialized = true;
	}

	g->IsInitialized = false;

	int sdlFlags = SDL_WINDOW_RESIZABLE;
	if (g->cachedConfig.Fullscreen)
	{
		sdlFlags |= SDL_WINDOW_FULLSCREEN;
	}

	const int w = g->cachedConfig.Res.x;
	const int h = g->cachedConfig.Res.y;

	if (!force && !g->cachedConfig.IsEditor)
	{
		g->modeIndex = FindValidMode(g, w, h, g->cachedConfig.ScaleFactor);
		if (g->modeIndex == -1)
		{
			g->modeIndex = 0;
			LOG(LM_MAIN, LL_ERROR, "invalid Video Mode %dx%d\n", w, h);
			return;
		}
	}

	LOG(LM_MAIN, LL_INFO, "graphics mode(%dx%d %dx)",
		w, h, g->cachedConfig.ScaleFactor);
	SDL_DestroyTexture(g->screen);
	SDL_DestroyRenderer(g->renderer);
	SDL_FreeFormat(g->Format);
	SDL_DestroyWindow(g->window);
	SDL_CreateWindowAndRenderer(w, h, sdlFlags, &g->window, &g->renderer);
	if (g->window == NULL || g->renderer == NULL)
	{
		LOG(LM_MAIN, LL_ERROR, "cannot create window or renderer: %s\n",
			SDL_GetError());
		return;
	}
	char title[32];
	debug(D_NORMAL, "setting caption and icon...\n");
	sprintf(title, "C-Dogs SDL %s%s",
		g->cachedConfig.IsEditor ? "Editor " : "",
		CDOGS_SDL_VERSION);
	SDL_SetWindowTitle(g->window, title);
	SDL_SetWindowIcon(g->window, g->icon);
	g->Format = SDL_AllocFormat(SDL_GetWindowPixelFormat(g->window));
	if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear"))
	{
		LOG(LM_MAIN, LL_WARN, "cannot set render quality hint: %s\n",
			SDL_GetError());
	}
	if (SDL_RenderSetLogicalSize(g->renderer, w, h) != 0)
	{
		LOG(LM_MAIN, LL_ERROR, "cannot set renderer logical size: %s\n",
			SDL_GetError());
		return;
	}
	g->screen = SDL_CreateTexture(
		g->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
		w, h);
	if (g->screen == NULL)
	{
		LOG(LM_MAIN, LL_ERROR, "cannot create screen texture: %s\n",
			SDL_GetError());
		return;
	}
	// Values for ARGB8888
	g->Amask = 0xFF000000;
	g->Ashift = 24;

	CFREE(g->buf);
	CCALLOC(g->buf, GraphicsGetMemSize(&g->cachedConfig));
	CFREE(g->bkg);
	CCALLOC(g->bkg, GraphicsGetMemSize(&g->cachedConfig));

	debug(D_NORMAL, "Changed video mode...\n");

	GraphicsSetBlitClip(
		g, 0, 0, g->cachedConfig.Res.x - 1, g->cachedConfig.Res.y - 1);
	debug(D_NORMAL, "Internal dimensions:\t%dx%d\n",
		g->cachedConfig.Res.x, g->cachedConfig.Res.y);

	g->IsInitialized = true;
	g->cachedConfig.Res.x = w;
	g->cachedConfig.Res.y = h;
	g->cachedConfig.needRestart = false;
}

void GraphicsTerminate(GraphicsDevice *g)
{
	debug(D_NORMAL, "Shutting down video...\n");
	CArrayTerminate(&g->validModes);
	SDL_FreeSurface(g->icon);
	SDL_DestroyTexture(g->screen);
	SDL_DestroyRenderer(g->renderer);
	SDL_FreeFormat(g->Format);
	SDL_DestroyWindow(g->window);
	SDL_VideoQuit();
	CFREE(g->buf);
	CFREE(g->bkg);
}

int GraphicsGetScreenSize(GraphicsConfig *config)
{
	return config->Res.x * config->Res.y;
}

int GraphicsGetMemSize(GraphicsConfig *config)
{
	return GraphicsGetScreenSize(config) * sizeof(Uint32);
}

void GraphicsConfigSet(
	GraphicsConfig *c,
	const Vec2i res, const bool fullscreen, const int scaleFactor)
{
	if (!Vec2iEqual(res, c->Res))
	{
		c->Res = res;
		c->needRestart = true;
	}
#define SET(_lhs, _rhs) \
	if ((_lhs) != (_rhs)) \
	{ \
		(_lhs) = (_rhs); \
		c->needRestart = true; \
	}
	SET(c->Fullscreen, fullscreen);
	SET(c->ScaleFactor, scaleFactor);
}

void GraphicsConfigSetFromConfig(GraphicsConfig *gc, Config *c)
{
	GraphicsConfigSet(
		gc,
		Vec2iNew(
			ConfigGetInt(c, "Graphics.ResolutionWidth"),
			ConfigGetInt(c, "Graphics.ResolutionHeight")),
		ConfigGetBool(c, "Graphics.Fullscreen"),
		ConfigGetInt(c, "Graphics.ScaleFactor"));
}

char *GrafxGetModeStr(void)
{
	static char buf[16];
	sprintf(buf, "%dx%d %dx",
		ConfigGetInt(&gConfig, "Graphics.ResolutionWidth"),
		ConfigGetInt(&gConfig, "Graphics.ResolutionHeight"),
		ConfigGetInt(&gConfig, "Graphics.ScaleFactor"));
	return buf;
}

void GraphicsSetBlitClip(
	GraphicsDevice *device, int left, int top, int right, int bottom)
{
	device->clipping.left = left;
	device->clipping.top = top;
	device->clipping.right = right;
	device->clipping.bottom = bottom;
}

void GraphicsResetBlitClip(GraphicsDevice *device)
{
	GraphicsSetBlitClip(
		device,
		0, 0,
		device->cachedConfig.Res.x - 1,
		device->cachedConfig.Res.y - 1);
}
