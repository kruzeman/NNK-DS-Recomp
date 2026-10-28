# gxrender — DS GX command decoder + rasterizer (Ni no Kuni DS, B2KJ)

Hardware-accelerated 3D render track foundation for the Ni no Kuni DS static
recompilation.  C99, integer-only pipeline (byte-deterministic output).

Layout:

    src/gx_types.h     fixed-point conventions (1.19.12, 1/16 px, 12.4 uv)
    src/gx_matrix.c    4x4 matrix stack ops (melonDS-exact arithmetic)
    src/gx_fifo.c      GX FIFO decoder + vertex pipeline + polygon assembly
    src/gx_tex.c       DS texture decode (2/4/8bpp, A3I5, A5I3, 15bpp)
    src/gx_raster.c    barycentric rasterizer, DS shading modes, PPM writer
    src/gx_gl.c        SDL2+OpenGL backend skeleton
    tests/             unit tests (hand-computed expectations) + golden test
    tools/             cube golden test, GL smoke test, shared scene builder
    GX_COVERAGE.md     implemented vs stubbed command table

Build / test:

    make            # library + tests + tools
    make test       # unit tests + determinism golden test (3 + 3 runs)
    make golden     # compare against committed tests/golden/cube.ppm
    make gl-smoke   # GL backend smoke under xvfb-run
    make check      # everything

Reference semantics: GBATEK (DS hardware) validated against melonDS sources
at /home/hermes/tools/melonDS (GPU3D.cpp, GPU3D_Soft.cpp).  See
GX_COVERAGE.md for the exact coverage/stub status.
