CC ?= cc
CFLAGS ?= -O2 -g -std=c99 -Wall -Wextra
CPPFLAGS += -Isrc -Itools

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null) -lGL

BUILD := build
LIB := $(BUILD)/libgxrender.a
OBJ := $(BUILD)/gx_matrix.o $(BUILD)/gx_tex.o $(BUILD)/gx_raster.o $(BUILD)/gx_fifo.o
GL_OBJ := $(BUILD)/gx_gl.o

TESTS := $(BUILD)/test_matrix $(BUILD)/test_tex $(BUILD)/test_shade
TOOLS := $(BUILD)/gx_cube_test $(BUILD)/gx_gl_smoke

all: $(LIB) $(GL_OBJ) $(TESTS) $(TOOLS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/gx_gl.o: src/gx_gl.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(LIB): $(OBJ)
	ar rcs $@ $(OBJ)

$(BUILD)/test_matrix: tests/test_matrix.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB)

$(BUILD)/test_tex: tests/test_tex.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB)

$(BUILD)/test_shade: tests/test_shade.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB)

$(BUILD)/gx_cube_test: tools/gx_cube_test.c tools/gx_scene.h $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/gx_cube_test.c $(LIB)

$(BUILD)/gx_gl_smoke: tools/gx_gl_smoke.c tools/gx_scene.h $(LIB) $(GL_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL_CFLAGS) -o $@ tools/gx_gl_smoke.c $(GL_OBJ) $(LIB) $(SDL_LIBS)

# unit tests + golden determinism test (3 in-process runs + 3 separate runs)
test: all
	mkdir -p tests/out
	$(BUILD)/test_matrix
	$(BUILD)/test_tex
	$(BUILD)/test_shade
	$(BUILD)/gx_cube_test --runs 3 --out tests/out/cube.ppm
	sh tests/check_determinism.sh

# compare the current renderer output against the committed golden PPM
golden: all
	mkdir -p tests/out
	$(BUILD)/gx_cube_test --runs 1 --out tests/out/cube.ppm
	cmp tests/golden/cube.ppm tests/out/cube.ppm && echo "golden match"

# GL backend smoke test (software GL via Xvfb is fine; see GX_COVERAGE.md)
gl-smoke: $(BUILD)/gx_gl_smoke
	mkdir -p tests/out
	xvfb-run -a $(BUILD)/gx_gl_smoke --out tests/out/cube_gl.ppm

check: test golden gl-smoke

clean:
	rm -rf $(BUILD) tests/out

.PHONY: all test golden gl-smoke check clean
