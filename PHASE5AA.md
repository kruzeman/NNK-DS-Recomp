# Phase-5AA — the intro ADVANCES: two concurrent movies, one per screen

Follows Phase-5W (`35fb674`).  Phase-5W left the intro static: the "session"
alternated between two codec structs, the HLE reset ONE global frame index on
every switch, so every present showed stream frame 0, and the run halted at
`halt_pc=0x0207B218`, step 2,280,211,542.

This phase measured what the two "sessions" are, replaced the global cursor
with per-codec stream state, and re-measured the halt.  Result: the intro now
advances frame by frame on both screens and the run passes the old halt — it
ran to the 2.45e9 step budget.

## 1. The two codecs are two concurrent movies (melonDS ground truth)

Probe: `MELONDS_REGPROBE_PCS=0x0207B218` (40 hits), with the 0x60-byte player
headers dumped at every hit.  The call site is overlay 13's decode driver
`0x0207B1D0`: `[r5+0x34]` = codec, `[r5+0x38]` = callback, `blx r1` at
`0x0207B218`, cursor `[codec] += r0` at `0x0207B21C`.

```
hit 1  ts=2272165619 r0=0x0227834C r1=0x01FF8540 r5=0x02278264
hit 2  ts=2272246861 r0=0x022E8B74 r1=0x01FF8540 r5=0x022E8A8C
hit 3  ts=2272646630 r0=0x0227834C r1=0x01FF8540 r5=0x02278264
...
hit 40 ts=2307573062 r0=0x022E8B74 r1=0x01FF8540 r5=0x022E8A8C
```

* `r1` is **always** `0x01FF8540` (the ITCM decode callback) — never NULL.
* Two player objects / two codecs alternate: `0x02278264`/`0x0227834C` and
  `0x022E8A8C`/`0x022E8B74`.  Each player embeds the MODS header at `+4`
  (`0x53444F4D` "MODS", `0x000A334E` "N3\n\0"), and the owner relation is exact:
  `[player+0x34] == codec`, `[player+0x38] == 0x01FF8540`, `player == codec-0xE8`.
* `[player+0x5C]`/`[player+0x60]` point at arrays of six Y/chroma plane
  pointers; those six Y entries **are** the codec's six ring slots
  (`codec+0x0C..0x20`), selected by `[player+0xC4]`.

Which movie is which (melonDS main RAM at `ts=2,617,9xx,xxx`, Phase-5P dump;
every ring slot matched against the ffmpeg decode):

| player | ring slot | best frame | MAE |
|---|---|---|---|
| `0x02278264` (codec `0x0227834C`) | `0x022C0880` | `op_u` f0130 | 0.000 |
| `0x022E8A8C` (codec `0x022E8B74`) | `0x023430C8` | `op_d` f0125 | 0.000 |
| | `0x022E9028` | `op_d` f0126 | 0.000 |
| | `0x022FB048` | `op_d` f0127 | 0.000 |
| | `0x0230D068` | `op_d` f0128 | 0.000 |
| | `0x0231F088` | `op_d` f0129 | 0.000 |
| | `0x023310A8` | `op_d` f0130 | 16.9 (in flight) |

⇒ the intro decodes **op_u for the top screen and op_d for the bottom screen,
concurrently and frame-synchronised** (both rings end at f0130 at 11 s).

## 2. Fix: one stream per codec (`tools/nnk_smc.inc`)

* `P5tStream p5t_str[4]`, keyed by the callback's `r0` (`p5t_stream(codec)`);
  a codec seen at a new address is a new session at frame 0.
* per stream: `index`, `cursor`, `frames`, `presents`, `FILE *fp`.
* source 1 = `NNK_MOVIE_YUV` (op_u), source 2 = `NNK_MOVIE_YUV2` (op_d).
* frame index = decode steps of THAT stream (a plain increment; the measured
  stream cursor is not monotonic — it oscillates between two read positions
  0x022E48E8 / 0x022E6A88 / 0x022E4A58 / 0x022E6B28 — so it cannot be used as
  a session-reset detector).
* the present writes **only the current codec's six Y ring slots** — the
  decoder's real output buffers.  The player's `+0x3C/+0x40` are filled by the
  guest's own present function (`0x0207B2AC`) from `[player+0x5C]/[0x60]`
  right before it calls the converter, so the HLE must not write them (that
  stale field is the Phase-5W corruption path).

Measured effect (final binary, `NNK_MOVIE_YUV=op_u.yuv`,
`NNK_MOVIE_YUV2=op_d.yuv`):

```
movie: present stream=0 codec=0x0227834C n=1 frame=0 cursor=0x022E48E8 ...
movie: present stream=1 codec=0x022E8B74 n=1 frame=0 cursor=0x02355110 ...
movie: present stream=0 ... n=2 frame=1 ... n=3 frame=2 ... n=4 frame=3 ...
movie: present stream=1 ... n=2 frame=1 ... n=3 frame=2 ... n=4 frame=3 ...
```

Both streams advance monotonically, one host frame per decode step.

## 3. Display evidence (top screen, /home/hermes/NiNoKuni/shots/)

Captures every 5 display frames, matched against the ffmpeg decode of op_u
(the reference plane melonDS's ring slot `0x022C0880` holds byte-exactly):

```
f.04130.top.ppm  best ref f0018  corr 0.9974  lumaMAE 2.6  -> p5aa.04130_top.png
f.04160.top.ppm  best ref f0022  corr 0.9968  lumaMAE 2.4  -> p5aa.04160_top.png
f.04200.top.ppm  best ref f0027  corr 0.9979  lumaMAE 3.8  -> p5aa.04200_top.png
```

Three successive DIFFERENT clean movie frames, advancing f18 → f22 → f27
(frames 4060/4080/4100 are the pre-movie black screen, as the movie starts at
display frame ~4045).  This is the "frame-diff vs melonDS" check: melonDS's
own ring plane for the same window is byte-exact to the same op_u frames
(Phase-5T, corr 1.0000), and the guest's unmodified YCoCg converter is what
turns the HLE plane into these screens (corr ≈ 0.997 against the reference).

## 4. The halt at 2,280,211,542 (B) — passed, root cause named

The task's premise ("no decode coverage at `0x0207B218`") is false.  The
failing PC is 0:

```
untranslated pc=0x00000000
done steps=2280211542 ... halt_pc=0x0207B218 halt=dispatch to untranslated pc
halt state: r0=00000000 r1=00000000 r5=022E8A8C r6=022E8A8C r7=0000000B
            r14=0207B21C
```

`halt_pc` is `last_pc`; `r14=0x0207B21C` (link written by the `blx`) and `r1=0`
mean **`blx r1` with a NULL callback**, because the whole bottom player
(`0x022E8A8C`) and its codec (`0x022E8B74`) read **all zero** in native RAM at
the halt.  melonDS at the same driver call (hit 23: `r7=0x0B`, `ts≈2.288e9`)
has `[B+0x34]=0x022E8B74` and `[B+0x38]=0x01FF8540` intact — the block is not
supposed to be dead.

With the §2 present path the halt is GONE: the final run executed to
`done steps=2450000001 halt=step budget exhausted` (2.45e9), i.e.
169,788,459 steps past the old halt, with both streams presenting.

Residual (honest): the exact write that zeroed the block in the old run is not
individually isolated.  Storewatch on the B block in the fixed run records only
its initialisation (1116 stores, all at step 2,255,965,038, pc 0x02029Cxx —
the `memset(codec,0,size)` + field init) and no later writer, so the block is
never touched again once the HLE stops rewriting the same all-zero frame 0 into
the session and stops writing the player's source-plane fields.  Bounded
follow-up if it ever recurs: run `NNK_STOREWATCH="0x022E8A8C:0x022E8B80"` on
the pre-5AA binary and check whether the writer is a guest store or the HLE
copy (the HLE memcpy bypasses the store funnel, which is why Phase-5W needed
the arena guard).

## 5. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 38 passed; 0 failed   (class 38 added)
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh      # see run log
```

Class 38 (`movie_decode_progress_is_tracked_per_codec`) locks the per-codec
stream table, the plain per-stream index, the second source env, the
player-field write ban and the callback-return hook site.
