// Phase 1 hello-world: prove the emscripten SDL2 toolchain works.
//
// Draws a filled cyan rectangle on a black canvas, moving diagonally
// so we can visually confirm the main loop is running.
//
// Build: see build_hello.sh in this directory.

#include <SDL.h>
#include <emscripten.h>
#include <cstdio>

struct app_state {
	SDL_Window* wnd = nullptr;
	SDL_Renderer* ren = nullptr;
	int x = 0;
	int y = 0;
	int dx = 4;
	int dy = 3;
	int width = 640;
	int height = 480;
	int rect_size = 60;
};

static app_state state;

extern "C" void frame() {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) emscripten_cancel_main_loop();
	}

	state.x += state.dx;
	state.y += state.dy;
	if (state.x < 0 || state.x + state.rect_size > state.width) state.dx = -state.dx;
	if (state.y < 0 || state.y + state.rect_size > state.height) state.dy = -state.dy;

	SDL_SetRenderDrawColor(state.ren, 0, 0, 0, 255);
	SDL_RenderClear(state.ren);
	SDL_SetRenderDrawColor(state.ren, 0, 200, 220, 255);
	SDL_Rect r{state.x, state.y, state.rect_size, state.rect_size};
	SDL_RenderFillRect(state.ren, &r);
	SDL_RenderPresent(state.ren);
}

int main() {
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	state.wnd = SDL_CreateWindow("openBW WASM hello",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		state.width, state.height, 0);
	if (!state.wnd) {
		std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	state.ren = SDL_CreateRenderer(state.wnd, -1, 0);
	if (!state.ren) {
		std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		return 1;
	}

	std::fprintf(stderr, "openBW hello: canvas %dx%d, starting main loop\n",
		state.width, state.height);
	emscripten_set_main_loop(frame, 0, 1);
	return 0;
}
