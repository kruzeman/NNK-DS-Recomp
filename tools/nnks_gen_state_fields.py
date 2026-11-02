#!/usr/bin/env python3
"""Generate nnk_state_fields.h — the field table of the Phase-5AQ savestate.

A savestate must carry EVERY mutable static object of the runtime prelude
(CPU, banked registers, IO/PPU model, card model, SMC generation tables, input
script position, movie stream cursors, ...).  Enumerating that by hand rots:
a counter added in a later phase would silently diverge a restored run, which
is exactly the failure mode a savestate exists to rule out.

So the list is DERIVED from the effective prelude at build time:
  $SPLIT/nnk_main.c           everything before `static void run(void)` (the
                              image prelude + the injected hooks)
  $SPLIT/nnk_dma.inc          timer/DMA/IPC shims
  $SPLIT/nnk_vramcnt.inc      VRAMCNT model + 2D shadow
  $SPLIT/nnk_input.inc        key state + input scripts
  $SPLIT/nnk_smc.inc          SMC generation state, movie HLE cursors, probes
  $SPLIT/nnk_boot.inc         firmware boot seeding

Objects NOT carried are host-owned and therefore rebound/re-derived by the
loader (they never belong in a file that must be portable between processes):
  gxd (SDL/GL handle), gxd_shot, rom, smc_img, smc_seg[].data,
  smc_seen_key/smc_seen_form, nnk_in_log, nnk_rec, storewatch_fp,
  p5t_str[].fp, p5t_str[].dec (codec references — re-decoded from frame 0 on
  load), p5t_buf (decode scratch), and every pointer / FILE* / SDL object.

usage: tools/nnks_gen_state_fields.py <split-dir> > <split-dir>/nnk_state_fields.h
"""
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import nnks_list_statics as L  # noqa: E402

RAM = {'gmem'}
ITCM = {'itcm'}
DTCM = {'dtcm'}
CARD = {'card_cmd', 'spicnt', 'romcnt', 'rom_loaded', 'card_cycles',
        'card_next', 'card_end_pending', 'card_drq', 'card_addr', 'card_len',
        'card_pos', 'card_fifo', 'card_poscpu', 'card_poscart', 'card_count',
        'card_late', 'card_xfer', 'card_gap2', 'card_op', 'card_poll_n',
        'card_drq_log_n', 'card_xfer_log_n'}
SMC = {'smc_armed', 'smc_f8log', 'smc_dirty', 'smc_gen', 'smc_icnt',
       'smc_dblk', 'smc_band_seg', 'smc_ntry', 'smc_nhit', 'smc_ninsn',
       'smc_nhalt', 'smc_nfb', 'smc_nany', 'smc_trace_left', 'smc_trace_from',
       'smc_t_sticky', 'smc_t_sticky_step', 'smc_t_pc', 'smc_t_val',
       'smc_t_step', 'smc_pcbase_reg', 'smc_pcbase_step', 'smc_stconf',
       'smc_nmap', 'smc_nconf', 'smc_nflip', 'smc_nhole', 'smc_hole_pc',
       'smc_nhole_pc', 'smc_nlive', 'smc_live_pc', 'smc_nlive_pc',
       'smc_seg_n', 'storewatch_state', 'storewatch_lo', 'storewatch_hi',
       'storewatch_mask', 'storewatch_n', 'storewatch_limit', 'f8n',
       'seen', 'nseen', 'p5u_state', 'p5u_from', 'p5u_every', 'p5u_next',
       'p5u_capture', 'p5u_samples', 'p5u_top_n', 'p5u_cb_in', 'p5u_cb_ret',
       'p5u_hpc', 'p5u_hit'}
MOVIE = {'p5t_state', 'p5t_nstr', 'p5t_codec'}
INPUT = {'nnk_mask', 'nnk_extkey', 'nnk_inited', 'nnk_ki_reads', 'nnk_ki_last',
         'nnk_sc', 'nnk_inj', 'nnk_inj_n', 'nnk_inj_next', 'nnk_script',
         'nnk_script_n', 'nnk_script_next', 'nnk_script_mode',
         'nnk_file_parsed', 'nnk_file_mtime', 'nnk_file_size',
         'nnk_file_frame', 'nnk_script_applied', 'nnk_rec_open',
         'nnk_rec_last', 'nnk_rec_frame', 'nnk_rec_entries',
         'nnk_rec_first_frame', 'p4x_band_log'}
DISPLAY = {'gxd_frames', 'gxd_last_vc', 'gxd_shot_every'}

# Anonymous-struct static arrays: the generator cannot name their type, and
# they are constant after parsing OR plain POD that the input script owns.
EXTRA = ['nnk_inj', 'nnk_script', 'nnks_pc']

# RUN CONFIGURATION, not emulated state: these come from the environment of
# the process that is running now, so restoring the saved values would
# override the current run's flags (a restored state must never change
# NNK_STEP_LIMIT, a probe address, or a trace gate).
CONFIG = {'step_limit', 'trace_from', 'mode_trace', 'gxd_shot_every',
          'dbg_pc', 'dbg_hits', 'dbg_limit', 'halt_at_pc', 'ckpt_pc',
          'ckpt_done', 'itcm_dumped', 'pred_pc', 'pred_npc', 'pred_hits',
          'pred_limit', 'pred_rows', 'dc_pc', 'dc_npc', 'dc_hits',
          'dc_limit', 'dc_words', 'p5u_state', 'p5u_from', 'p5u_every',
          'p5u_next', 'p5u_capture', 'p5u_samples', 'p5u_top_n',
          'p5u_cb_in', 'p5u_cb_ret', 'p5u_hpc', 'p5u_hit',
          'storewatch_state', 'storewatch_lo', 'storewatch_hi',
          'storewatch_mask', 'storewatch_n', 'storewatch_limit',
          'smc_trace_left', 'smc_trace_from', 'smc_f8log', 'smc_stconf',
          # Phase-5AS: the carried-state override handoff (`smc_force_state`)
          # lives for exactly one dispatch and `smc_ncarry` is its diagnostic
          # counter — neither is machine state, and keeping both out of the
          # payload keeps this build's state layout byte-identical to the
          # pre-5AS ones (the operator's bug reproducer is such a state).
          'smc_force_state', 'smc_ncarry', 'smc_t_live', 'smc_carrier_ab',
          'nnk_vrc_trace', 'nnk_vrc_win', 'nnk_vrc_win5', 'nnk_vrc_win_first',
          'nnk_vrc_win_last', 'nnk_p4s_log', 'nnk_p4r_drop_log',
          'nnk_p4r_drain_log', 'nnk_p4s_done', 'nnk_p4s_first',
          'nnk_dma_shim_n', 'nnk_dma_timed_n', 'nnk_inited',
          # function-local statics of the diagnostics / boot seeding: not
          # reachable from the field table (and not machine state)
          'f8n', 'nseen', 'def_mac', 'p', 'vc', 'init', 'i', 'n2', 'j'}


def classify(name):
    for region, names in (('ram', RAM), ('itcm', ITCM), ('dtcm', DTCM),
                          ('card', CARD), ('smc', SMC), ('movie', MOVIE),
                          ('input', INPUT), ('display', DISPLAY)):
        if name in names:
            return region
    return 'io'


def collect(split):
    files = [split + '/nnk_main.c', split + '/nnk_dma.inc',
             split + '/nnk_vramcnt.inc', split + '/nnk_input.inc',
             split + '/nnk_smc.inc', split + '/nnk_boot.inc']
    out = []
    seen = set()
    for f in files:
        try:
            names = L.scan(f, stop_at_run=f.endswith('nnk_main.c'))
        except OSError:
            continue
        for n in names:
            if n in seen or n in CONFIG:
                continue
            seen.add(n)
            out.append(n)
    for n in EXTRA:
        if n not in seen:
            seen.add(n)
            out.append(n)
    return out


def main():
    split = sys.argv[1] if len(sys.argv) > 1 else 'work/split/c'
    names = collect(split)
    regions = {}
    for n in names:
        regions.setdefault(classify(n), []).append(n)
    w = sys.stdout.write
    w('/* GENERATED by tools/nnks_gen_state_fields.py — do not edit.\n'
      ' *\n'
      ' * The savestate field table (Phase-5AQ).  One row per mutable static\n'
      ' * object of the effective prelude, grouped into payload regions.\n'
      ' * Every row is serialized verbatim: adding a runtime static adds a\n'
      ' * row automatically, so a restored state cannot silently miss it.\n'
      ' * See SAVESTATES.md for the format and the NOT-CARRIED list.\n'
      ' */\n')
    w('#ifndef NNK_STATE_FIELDS_H\n#define NNK_STATE_FIELDS_H\n\n')
    for region in ('io', 'smc', 'card', 'input', 'movie', 'display', 'ram',
                   'itcm', 'dtcm'):
        rows = regions.get(region, [])
        if not rows:
            continue
        w('static const NnkStateField nnks_fields_%s[] = {\n' % region)
        if region in ('input', 'display'):
            w('#ifdef NNK_GX\n')
        for n in rows:
            w('    NNKS_F(%s),\n' % n)
        if region in ('input', 'display'):
            w('#endif\n')
        w('};\n\n')
    w('#endif /* NNK_STATE_FIELDS_H */\n')


if __name__ == '__main__':
    main()
