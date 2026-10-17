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
 * Current: ag_mahal_pm_th.h, AG_MAHAL_MODEL_VERSION
 *   "ml3-v1+pm_th+b07dd6863436", data=synthetic (ข้อมูลสังเคราะห์
 *   ไม่ใช่ค่าวัดจริง), made on 17 Oct 2026 with
 *   python -m ml.baseline --feature-set pm_th --out data/processed/ml3
 *   sha256 of the file 931c1f91f8cbcdd7e601e052e1c44f240baddcf3155b863392c45bd05cf18b56 */
#include "ag_mahal_pm_th.h"
