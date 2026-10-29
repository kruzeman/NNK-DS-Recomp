CC ?= cc
CFLAGS ?= -O2 -g -std=c99 -Wall -Wextra
CPPFLAGS += -Isrc -Itools

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null) -lGL

BUILD := build
LIB := $(BUILD)/libgxrender.a
OBJ := $(BUILD)/gx_matrix.o $(BUILD)/gx_tex.o $(BUILD)/gx_raster.o $(BUILD)/gx_fifo.o $(BUILD)/gx_2d.o
GL_OBJ := $(BUILD)/gx_gl.o

TESTS := $(BUILD)/test_matrix $(BUILD)/test_tex $(BUILD)/test_shade $(BUILD)/test_post $(BUILD)/test_2d
TOOLS := $(BUILD)/gx_cube_test $(BUILD)/gx_gl_smoke $(BUILD)/gx_replay $(BUILD)/gx_2dreplay $(BUILD)/gx_2d_golden

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
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_shade.c $(LIB)

$(BUILD)/test_post: tests/test_post.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_post.c $(LIB)

$(BUILD)/test_2d: tests/test_2d.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_2d.c $(LIB)

$(BUILD)/gx_2dreplay: tools/gx_2dreplay.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/gx_2dreplay.c $(LIB)

$(BUILD)/gx_2d_golden: tools/gx_2d_golden.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/gx_2d_golden.c $(LIB)

$(BUILD)/gx_cube_test: tools/gx_cube_test.c tools/gx_scene.h $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/gx_cube_test.c $(LIB)

$(BUILD)/gx_gl_smoke: tools/gx_gl_smoke.c tools/gx_scene.h $(GL_OBJ) $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL_CFLAGS) -o $@ tools/gx_gl_smoke.c $(GL_OBJ) $(LIB) $(SDL_LIBS)

$(BUILD)/gx_replay: tools/gx_replay.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/gx_replay.c $(LIB)

# unit tests + golden determinism test (3 in-process runs + 3 separate runs)
test: all
	mkdir -p tests/out
	$(BUILD)/test_matrix
	$(BUILD)/test_tex
	$(BUILD)/test_shade
	$(BUILD)/test_post
	$(BUILD)/test_2d
	$(BUILD)/gx_cube_test --runs 3 --out tests/out/cube.ppm
	sh tests/check_determinism.sh

# compare the current renderer output against the committed golden PPM
golden: all
	mkdir -p tests/out
	$(BUILD)/gx_cube_test --runs 1 --out tests/out/cube.ppm
	cmp tests/golden/cube.ppm tests/out/cube.ppm && echo "golden match"

# 2D compositor golden + determinism (gx_2d_golden vs tests/golden/scene2d.ppm)
golden2d: $(BUILD)/gx_2d_golden
	mkdir -p tests/out
	$(BUILD)/gx_2d_golden --out tests/out/scene2d.ppm
	$(BUILD)/gx_2d_golden --out tests/out/scene2d_b.ppm
	cmp tests/out/scene2d.ppm tests/out/scene2d_b.ppm && echo "2D determinism match"
	cmp tests/golden/scene2d.ppm tests/out/scene2d.ppm && echo "2D golden match"

# GL backend smoke test (software GL via Xvfb is fine; see GX_COVERAGE.md)
gl-smoke: $(BUILD)/gx_gl_smoke
	mkdir -p tests/out
	xvfb-run -a $(BUILD)/gx_gl_smoke --out tests/out/cube_gl.ppm

check: test golden golden2d gl-smoke

clean:
	rm -rf $(BUILD) tests/out

.PHONY: all test golden golden2d gl-smoke check clean
