# ML-1 — ข้อมูลสังเคราะห์ + ตัวโหลด + แบ่งตามเวลา

**branch:** `ml/synth-load-split` · **ต้องรอ:** — · ทำได้ทันทีโดยไม่ต้องมีบอร์ด

## เป้าหมาย

สร้างข้อมูลปลอมที่ **พิสูจน์โจทย์ของโปรเจกต์ได้** (ทำอาหาร = ปกติ · ปัดฝุ่น = ผิดปกติ) ในรูปแบบเดียวกับที่บอร์ดจะเขียนจริง
พร้อมตัวโหลดและการแบ่งข้อมูลที่ **ทำผิดกฎ §2 ไม่ได้เพราะ test จะล้ม**

## อ่านก่อน

- `CLAUDE.md` §1 · **§2 หัวข้อ ML** · **§7 (สัญญา CSV)**
- `ml/README.md` (anomaly 3 แบบ · กฎ) · `data/README.md` (รูปแบบ events) · `docs/05-MULTIVARIATE.md` §1

## ไฟล์ที่สร้าง/แก้ — เท่านี้

```
ml/synth.py   ml/load.py   ml/split.py
ml/tests/test_synth.py   ml/tests/test_load.py   ml/tests/test_split.py
ml/conftest.py            ← ถ้าต้องใช้เพื่อให้ pytest import ได้
```
**ห้ามเพิ่ม dependency นอก `ml/requirements.txt`** · Python 3.11 (ตรงกับ CI)

## ข้อกำหนด

### `synth.py`
- `generate(days, seed, start, cfg) -> (raw_df, events_df)` + CLI
  `python ml/synth.py --days 7 --seed 42 --out data/processed/synth/`
- คอลัมน์และลำดับ **ตรง §7 ทุกตัว** · 1 แถว/วินาที · timestamp +07:00 · ทศนิยมตามตาราง §7
- ช่วงปกติต้องมี:
  - วัฏจักรรายวันของ T / RH ที่**สหสัมพันธ์เป็นลบ** (ห้องปิดเปิดแอร์ — `docs/05` §1)
  - **ช่วงทำอาหาร** ที่ PM + T + RH ขึ้นด้วยกัน → `label = normal` *(คือปกติตาม §1)*
  - `eco2_ppm` **คำนวณจาก `tvoc_ppb`** + noise เล็กน้อย ไม่ใช่สุ่มอิสระ (§7)
  - ค่าหายแบบสุ่มเป็นช่องว่าง (อัตราตั้งได้) — **ไม่ใช่ 0 หรือ -1**
- anomaly 3 แบบตาม `ml/README.md` → `label` = `spike` / `drift` / `decorrelation`
  - 🔑 **`decorrelation`**: PM ขึ้นขนาดเท่าช่วงทำอาหาร แต่ T / RH นิ่ง → **ทุกตัวแปรยังอยู่ในช่วงปกติเมื่อดูทีละตัว**
- `events_df` ตามรูปแบบ `data/README.md` (`start_iso,end_iso,label,note`) — เฉพาะ anomaly
- พารามิเตอร์ทั้งหมดอยู่ใน `dataclass` เดียว · คอมเมนต์ว่า **"สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง"**
- สุ่มด้วย `np.random.default_rng(seed)` เท่านั้น ห้ามใช้ global random

### `load.py`
- `load_raw(paths)` → DataFrame index เป็นเวลาแบบมี timezone · ช่องว่าง → `NaN`
- header ไม่ตรง §7 ทุกตัว → `ValueError` บอกว่าคอลัมน์ไหนผิด
- เรียงตามเวลา · timestamp ซ้ำ → `ValueError`
- `load_events(path)`

### `split.py`
- `chronological_split(df, t1, t2, gap_s)` → train / val / test **ตามเวลา** มีช่องว่าง `gap_s` คั่นกันรอยต่อ (กัน window คร่อม)
- `normal_only(df)` → เฉพาะ `label == normal` (เทรนด้วยข้อมูลปกติอย่างเดียว)
- `fit_normalizer(train) -> dict` · `save_normalizer(path)` เป็น `norm.json` · `apply_normalizer(df, params)`
  **ไม่มีฟังก์ชันไหนคำนวณ mean/std จาก val/test**

## test ที่ต้องมี — เป็นตัวบังคับกฎ ไม่ใช่ของประดับ

| test | ตรวจอะไร |
|---|---|
| schema | header ของ synth = รายการ §7 (**hardcode ใน test** ไม่ import จากโค้ดที่ทดสอบ) |
| reproducible | seed เดียวกัน → ไฟล์เหมือนกันทุกไบต์ · seed ต่าง → ต่าง |
| missing | ช่องว่างโหลดเป็น `NaN` ไม่ใช่ 0 |
| split ตามเวลา | `max(train) < min(val)` และ `max(val) < min(test)` · มีช่องว่าง ≥ `gap_s` |
| normalizer | เปลี่ยนค่าใน val/test ให้สุดโต่ง → พารามิเตอร์ไม่เปลี่ยน |
| 🔑 **decorrelation ซ่อนจาก 3σ** | ≥ 95% ของจุด `decorrelation` อยู่ใน mean ± 3σ ของ train ปกติ **ทุกตัวแปร** |
| spike มองเห็นได้ | จุด `spike` ส่วนใหญ่หลุด 3σ อย่างน้อยหนึ่งตัวแปร (sanity check) |

## ห้าม

- `train_test_split(..., shuffle=True)` หรือสุ่มแบ่งแบบใดก็ตาม
- คำนวณ normalizer จากข้อมูลทั้งชุด
- เขียนไฟล์ข้อมูลลงที่อื่นนอก `data/processed/` (ถูก ignore อยู่แล้ว)
- ทำงานของ ML-2 ขึ้นไป (metrics · Mahalanobis)

## เกณฑ์ผ่าน — คนรีวิวรันเองได้

```bash
pip install -r ml/requirements.txt
pytest ml/ -v                  # ผ่านทั้งหมด
ruff check ml/                 # ไม่มี error
python ml/synth.py --days 7 --seed 42 --out data/processed/synth/
```
รายงานต้องมี: จำนวนแถว · จำนวน event แต่ละแบบ · สัดส่วนจุด anomaly · ผล test ข้อ 🔑
