# ML-3: Baseline `ml3-v1` — 3σ vs Mahalanobis + export ให้ FW-7

```powershell
python -m ml.baseline --days 14 --seed 42 --out data/processed/ml3            # synthetic ทั้งสองชุด
python -m ml.baseline --feature-set pm_th --out data/processed/ml3            # เฉพาะ PM+T/RH
python -m ml.baseline --raw data/raw/AG_*.csv --events data/events/x.csv `
  --mapping "incense=anomaly,cooking=normal" --row-support-s 1 `
  --t1 2026-10-20T00:00:00+07:00 --t2 2026-10-22T00:00:00+07:00 --out data/processed/ml3-real
pytest ml/ -v ; ruff check ml/
```

ข้อมูล synthetic = **สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง** ผลบน synthetic
ไม่ใช่หลักฐานว่าโมเดลหรือเซนเซอร์ใช้ได้จริง

## 1. ชุด feature (ตั้งค่าได้)

| ชุด | คอลัมน์ (ลำดับ §7) | ใช้เมื่อ |
|---|---|---|
| `full` | `pm2_5, temp_c, humidity_pct, tvoc_ppb, co_ppm` | ENS160 + ZE07-CO พร้อม (FW-5) |
| `pm_th` | `pm2_5, temp_c, humidity_pct` | ก่อน FW-5 — คอลัมน์ tvoc/eco2/co ว่างทั้งหมดได้ |
| custom | `--feature-set pm2_5,temp_c` | ต้องเป็น subset ของคอลัมน์ตัวเลข §7 **เรียงตาม §7** |

ตัดออกโดยตั้งใจ ตาม `docs/05-MULTIVARIATE.md` §3.1: `pm1_0`/`pm10` (เซนเซอร์ตัวเดียวกับ PM2.5)
และ `eco2_ppm` (ENS160 คำนวณจาก TVOC ไม่ใช่ค่าอิสระ)
`co_ppm` เป็น input ของ ML ได้ แต่ **เส้นทางเตือน CO ยังแยกจาก ML โดยสิ้นเชิง** (CLAUDE.md §2) —
ถ้า ZE07 อ่านค่าคงที่ตลอด (std = 0) โมเดล `full` จะถูกปฏิเสธที่ขั้น condition number → ใช้ `pm_th`

## 2. Transform · normalization · missing policy

1. `log1p` กับ PM/TVOC (หางขวา, docs/05 §3.2) · identity กับ T/RH/CO
2. normalizer = `ml/split.py::fit_normalizer(train, columns=features)` fit บน **train ที่ label = normal เท่านั้น**
   (ค่า transform แล้ว) — ML-3 เพิ่มพารามิเตอร์ `columns` ให้ split.py รองรับ subset; ค่าเริ่มต้นเหมือน ML-1 ทุกอย่าง
3. **missing:** feature ที่เลือกตัวใดไม่ finite (หรือ input ของ log1p < 0) → score = missing
   ไม่ impute ไม่แทนศูนย์ · คอลัมน์ที่ไม่ได้เลือกว่างได้ไม่มีผล · missing รีเซ็ต persistence
   inf ใน train นับเป็น missing และรายงานใน `diagnostics.invalid_to_missing`

## 3. Mahalanobis และ covariance

```
z_j = (t_j − mean_j)/std_j          (normalizer จาก train normal)
d   = z − c                         (c = ค่าเฉลี่ยของ z บนแถว train normal ที่ครบทุก feature)
D²  = dᵀ P d,  P = (Σ*)⁻¹
```

ใช้ **D² (ยกกำลังสอง)** ทั้ง Python และ C ไม่ถอดราก

**ทำไม standardize ก่อน:** D² ไม่เปลี่ยนเมื่อสเกลแต่ละแกน แต่ (ก) shrinkage ไม่ scale-invariant
ทำบน z แล้วเป้าหมาย μI มีความหมาย (ข) cond(Σ_z) = cond ของ correlation matrix ซึ่งไม่ขึ้นกับหน่วย
ค่า cond ในหน่วยเดิม (`cond_raw_units_transformed`) รายงานไว้ด้วยแต่ใช้ตัดสินไม่ได้เพราะขึ้นกับว่า RH เป็น % หรือ log-PM

**Ledoit–Wolf shrinkage (2004)** บนแถวที่ center แล้ว x (n×p):

```
S = xᵀx/n,  μ = tr(S)/p,  δ² = ‖S − μI‖²_F / p
β̄² = (Σ_k ‖x_k‖⁴ − n‖S‖²_F) / (p n²)       ← มาจาก Σ_k ‖x_k x_kᵀ − S‖²_F / (p n²)
ρ = min(β̄², δ²)/δ²,   Σ* = ρμI + (1−ρ)S
```

เป็นสูตรเดียวกับ `sklearn.covariance.LedoitWolf` (venv ไม่มี scikit-learn จึงเขียนเองด้วย NumPy;
test เทียบกับนิยามแบบวนลูปตรง ๆ) · n ใหญ่ → ρ ≈ 0 → ไม่บิด covariance ที่ใช้จับ "ความสัมพันธ์ขาด"

**Condition gate:** ปฏิเสธเมื่อ `max(cond(S), cond(Σ*)) > 1e3`
- float32: eps = 2⁻²³ ≈ 1.19×10⁻⁷ · ความคลาดเคลื่อนสัมพัทธ์ของ quadratic form ≲ 2·p·eps·cond
  → p ≤ 8, cond ≤ 10³ ⇒ ≲ 2×10⁻³ · docs/05 §3.1: ρ = 0.999 → cond ≈ 2×10³ ใช้ไม่ได้แล้ว
- gate ดู **sample matrix** ด้วย เพราะเมื่อ n น้อย shrinkage ดึง cond ลงได้เองและจะซ่อน feature ซ้ำซ้อน
  (test `test_condition_gate_uses_configured_limit`: n = 2000, cond(S) > 10³ แต่ cond(Σ*) ≈ 217)
- **ไม่ใช้ pseudo-inverse:** pinv ให้น้ำหนัก **ศูนย์** กับทิศทางที่ variance ใกล้ศูนย์
  ซึ่งคือทิศทาง "ความสัมพันธ์ที่ไม่ควรขาด" — ความผิดปกติแบบ decorrelation จะหายไปจาก score
  แก้ที่ต้นเหตุ (ตัด feature) ดีกว่า

## 4. Threshold · persistence (validation เท่านั้น)

- persistence k: alarm เมื่อ score ≥ threshold ติดกัน k แถวที่ support ต่อเนื่อง
  Python ส่ง `min(score ของ k แถวล่าสุด)` เข้า `ml/metrics.py` (alarm ⇔ ทั้ง k แถวเกิน) —
  เทียบเท่าตัวนับบนบอร์ด: เกิน → +1 · ต่ำกว่า/missing/แถวขาด → 0 · alarm เมื่อ ≥ k
- สำหรับแต่ละ k ∈ {1, 3, 5, 10}: ไล่ threshold (float32) จากเหนือค่าสูงสุดของ val known-normal
  ลงตาม quantile 1−10⁻⁶ … 0.5 หยุดเมื่อ **false alarm episodes/วัน (ML-2) บน val > 1**
  (งบเตือนผิดเป็นสมมติฐานด้านการใช้งาน ไม่ใช่ค่ามาตรฐาน) เก็บ threshold ต่ำสุดก่อนเกินงบ;
  `stopped_by` บอกว่าหยุดเพราะงบหรือหมด candidate
- เลือก k: event detection rate (val) ↓ → MTTD ↑ → FA ↑ → k ↑ · val ไม่มี event → k เล็กสุด
- 3σ (`zmax = max_j |z_j|`) ใช้ขั้นตอนเดียวกัน + รายงานกฎตำรา `zmax ≥ 3, k = 1` (threshold_source = fixed)
- หลังเลือก: hash พารามิเตอร์ float32 + threshold + k (`frozen_sha256`) **ก่อน** score test ครั้งแรก
- χ²(p, α = 0.001) รายงานเป็นค่าอ้างอิงเท่านั้น ไม่ใช้ตั้ง threshold เพราะข้อมูลจริง/synthetic ไม่ normal

## 5. ประเมินผล

`ml/metrics.py` (`ml2-v1`) เท่านั้น: AP, precision/recall ณ threshold ที่ freeze, event detection,
MTTD คู่กับ detected/missed, false alarm episodes/วัน — **ไม่มี accuracy · ไม่มี point adjustment**
`ap` = AP ของ score หลัง persistence · `ap_raw_score_k1` = AP ของ score ดิบ
truth มาจาก events + mapping (raw `marked` ไม่ถูกตีเป็น anomaly เอง) · event ที่คร่อมขอบ split → error

## 6. ไฟล์ส่งมอบ (`data/processed/ml3/<set>/`)

| ไฟล์ | เนื้อหา |
|---|---|
| `ag_mahal_<set>.h` | C99 header: feature names, `AG_MAHAL_NUMERIC_INDEX` (0 = pm1_0 … 7 = co_ppm), transform, mean/std/center/precision (float32, `%.9g` round-trip ตรงบิต), `THRESHOLD_D2`, `PERSISTENCE_K`, tolerance, golden vectors, persistence sequence |
| `model.json` | ทุกค่าข้างบน + diagnostics (cond, eigenvalues, correlation, shrinkage) + ตาราง selection + split + χ² ref |
| `norm.json` | output ของ `split.save_normalizer` (ค่าใน transformed space) |
| `results.json` | ผล test ของทุกวิธี (ML-2 summary + per-event) |
| `test-mahalanobis/` | `metrics.json` + `points.csv` (audit ทุกแถว) |

**Golden vectors** = แถว §7 ดิบ (float32, `NAN` = ช่องว่าง) → D² อ้างอิง (float64 บนพารามิเตอร์ float32)
มาจาก validation + จุดสร้างขึ้น (center, ขอบ threshold ×0.5/×1/×2, missing, inf, PM ติดลบ, คอลัมน์ที่ไม่ใช้ว่าง)
**ไม่ใช้แถว test** · C ผ่านเมื่อ `|D²_c − D²_ref| ≤ 1e-3 + 1e-3·|D²_ref|` · ข้ามการเทียบ decision เมื่อ
`NEAR_THRESHOLD = 1` · pytest คอมไพล์ header ด้วย gcc (x86-64, float32) แล้วเทียบ — ผลบน ESP32-S3 ต้องวัดใน FW-7

อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์ ห้ามใช้แทน CO alarm ที่ได้มาตรฐาน UL 2034 หรือ EN 50291
