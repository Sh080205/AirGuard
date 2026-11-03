#pragma once

/* FW-7: which ML-3 model the firmware is built with. Include from
 * ag_mahal.c only (the model header defines static arrays).
 *
 * The included file is a byte-for-byte copy of the ML-3 export
 * (ml/BASELINE.md §6) — never edit it by hand. To switch models (e.g. to
 * one trained on real data): run ml.baseline, copy the new
 * data/processed/<out>/<set>/ag_mahal_<set>.h into this folder and change
 * the line below. The version string is logged at boot.
 *
 * FW-7 step 3 — swapping in the real-data model (ML R5) for the same
 * feature set: overwrite ag_mahal_pm_th.h with the new export, nothing
 * else. Every size, constant and fixture is read from that file
 * (_Static_assert + the boot self-test check it), the host test and the
 * firmware need no edit, and the "Current" note below is only history: the
 * boot line "mahal self-test PASS: model=..." is what tells which model a
 * board runs. A different feature set (e.g. full) = copy its file and
 * change the #include. After a swap: run test_host/run_tests.ps1 and
 * idf.py build before flashing.
 *
 * Current: ag_mahal_pm_th.h, AG_MAHAL_MODEL_VERSION
 *   "ml3-v1+pm_th+e6f29d2be5b3", data=real (ML R4, 3 Nov 2026: train
 *   25-29 Oct outside the box, ENS160 on, firmware cea807f; threshold
 *   D2 17.92, K 1), made with ml/RUNBOOK-REAL.md §4 into
 *   data/processed/ml-real/pm_th/
 *   sha256 of the file 3c2f12aea27c8ac2f6f1f2efdf25a5b9b43d28930523bbfb71b8d04b67df095c
 *   🔴 Trained outside the box: not valid for the device in its enclosure
 *   without retraining (CLAUDE.md §10, 23 Oct decision).
 * Before: "ml3-v1+pm_th+b07dd6863436", data=synthetic, 17 Oct 2026,
 *   sha256 931c1f91f8cbcdd7e601e052e1c44f240baddcf3155b863392c45bd05cf18b56 */
#include "ag_mahal_pm_th.h"
