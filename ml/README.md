# ml — คน C

Python · anomaly detection แบบ unsupervised · จบที่ Mahalanobis D² บน ESP32-S3 (header ภาษา C ให้ FW-7)

> 📖 รูปแบบข้อมูลอยู่ใน [`../CLAUDE.md`](../CLAUDE.md) §7 — **ห้ามเปลี่ยนโดยไม่ตกลงกับคน B**
> 🗺️ แผนที่ไฟล์ทีละไฟล์และภาพรวมทั้งสาย (PC → บอร์ด) อยู่ใน [`../docs/11-CODE-MAP.md`](../docs/11-CODE-MAP.md) §8–9

---

## ลำดับงาน

สถานะตาม git (`main` @ `7e926b4` · 1 พ.ย. 2569) · แผน R0–R6 อยู่ใน [`../docs/10-ML-REAL-DATA-PROMPTS.md`](../docs/10-ML-REAL-DATA-PROMPTS.md) · คำสั่งจริงอยู่ใน [`RUNBOOK-REAL.md`](RUNBOOK-REAL.md)

| # | ไฟล์ / คำสั่ง | ทำอะไร | ต้องมีข้อมูลจริงไหม | สถานะ |
|---|---|---|---|---|
| ML-1 | `load.py` · `split.py` · `synth.py` | อ่าน CSV §7 · แบ่งตามเวลา · normalizer จาก train ที่ปกติ · ข้อมูลสังเคราะห์พร้อม anomaly 3 แบบ | ❌ | ✅ merge แล้ว |
| ML-2 | `metrics.py` · [`METRICS.md`](METRICS.md) | เมตริกแบบนับเหตุการณ์ (`ml2-v1`) | ❌ | ✅ merge แล้ว |
| ML-3 | `baseline.py` · [`BASELINE.md`](BASELINE.md) | 3σ vs Mahalanobis · เลือก threshold/K บน val · export `ag_mahal_<set>.h` | ❌ | ✅ merge แล้ว |
| R0 | `python -m ml.qa` | ตรวจคุณภาพไฟล์ raw (อ่านอย่างเดียว) | ✅ | ✅ โค้ด merge แล้ว |
| R1 | `python -m ml.events draft/check` | ร่างและตรวจไฟล์ events (ground truth) | ✅ | ✅ โค้ด merge แล้ว · ⬜ ไฟล์ events จริงยังไม่ได้ push |
| R2 | `python -m ml.dryrun` | ซ้อมทั้งสายด้วยข้อมูลสังเคราะห์ · ตรวจ header ด้วย gcc | ❌ | ✅ merge แล้ว |
| R3 | `ml.qa` + ตัดสิน | freeze t1 · ตัดสินเรื่องแถว U1–U4 ที่ไม่ได้กด MARK | ✅ | ⬜ รอข้อมูล |
| R4 | `python -m ml.baseline --raw …` | เทรน + วัดผล test **ครั้งเดียว** | ✅ | ⬜ รอข้อมูล |
| R5 | `python -m ml.dryrun check-header` → ก๊อปไป `firmware/components/mahal/model/` | ส่ง header ให้ FW-7 | ✅ | ⬜ |
| R6 | — | รูป/ตารางสำหรับสไลด์ | ✅ | ⬜ |
| (เสริม) | `train_ae.py` | autoencoder — ทำเมื่อ ML-3 + FW-7 เสร็จและมีเวลาเหลือ (CLAUDE.md §10) | — | ⬜ ยังไม่มีโค้ด |

> 🔑 ML-1 ถึง R2 ทำได้หมดโดยไม่ต้องมีข้อมูลจริง และเสร็จแล้ว · ตั้งแต่ R3 ต้องมีไฟล์ `data/raw/usb_train/AG_*_usb.csv` + `EV_*_usb.csv` ซึ่งไม่ขึ้น git (ต้องส่งต่อกันทางอื่น)

---

## anomaly 3 แบบที่ `synth.py` ต้องสร้าง

| แบบ | ลักษณะ | ตัวอย่างจริง |
|---|---|---|
| **ก. พุ่งทีเดียว** | ค่าเดียวพุ่งสูงแล้วลง | จุดธูป ทอดของ |
| **ข. ค่อยๆ ไต่** | ขึ้นช้าจนทีละจุดยังดูปกติ | ห้องปิด CO₂ สะสม |
| **ค. ความสัมพันธ์เพี้ยน** | ทุกตัวอยู่ในช่วงปกติ แต่ความสัมพันธ์ขาด | ปัดฝุ่น เชื้อราฟุ้ง |

> 🔴 **แบบ ค. คือเหตุผลทั้งหมดของโปรเจกต์** — ต้องแสดงให้ได้ว่า 3σ ทีละตัวจับไม่ได้
> แต่ Mahalanobis กับ autoencoder จับได้ **ถ้าพิสูจน์ข้อนี้ไม่ได้ โปรเจกต์ไม่มีเหตุผลรองรับ**

---

## 🔴 เมตริก — ข้อห้ามที่ทำให้งานเสียทั้งหมด

**ห้ามใช้ accuracy**
ข้อมูลไม่สมดุลรุนแรง — ทายว่า "ปกติ" ตลอดก็ได้ 99% แต่จับอะไรไม่ได้เลย

**ห้ามใช้ Point Adjustment**
Kim et al. (AAAI 2022) แสดงว่ามันทำให้**โมเดลสุ่มดูเหมือนชนะ state-of-the-art**
งานวิจัยจำนวนมากใช้แล้วผลเกินจริง — **ถ้าใช้ กรรมการที่รู้จะจับได้ทันที**

### ต้องใช้แทน

| เมตริก | ตอบคำถามอะไร |
|---|---|
| **PR-AUC** | คุณภาพโดยรวมเมื่อข้อมูลไม่สมดุล |
| **event-level detection rate** | จับเหตุการณ์ได้กี่ % *(นับเป็นเหตุการณ์ ไม่ใช่เป็นจุด)* |
| **MTTD** | เฉลี่ยกี่วินาทีกว่าจะจับได้หลังเหตุการณ์เริ่ม |
| **false alarm / วัน** | 🔑 **ตัวชี้วัดว่าคนจะใช้จริงไหม** — เตือนพร่ำเพรื่อ = ถูกถอดปลั๊ก |
| precision / recall | ที่ threshold ที่เลือกจริง |

---

## 🔴 กฎที่เหลือ

ML-2 implementation และนิยามที่ใช้จริงอยู่ใน [`METRICS.md`](METRICS.md)
PR-AUC ใช้ noninterpolated AP; MARK ที่ยังไม่ระบุชนิดเป็น unknown และ cooking
เป็น normal ผ่าน explicit mapping ดูตัวอย่างมือด้วย `python -m ml.metrics_example`
ผลเขียนใน `data/processed/metrics-example/` ไม่มีการ train โมเดล

ML-3 baseline (3σ vs Mahalanobis, ชุด feature `full`/`pm_th`, export header ให้ FW-7)
อยู่ใน [`BASELINE.md`](BASELINE.md) · รัน `python -m ml.baseline --out data/processed/ml3`

**แบ่ง train/val/test ตามเวลา ห้ามสุ่มแบ่ง**
time-series สุ่มแบ่ง = data leakage → ผลดูดีเกินจริงแล้วพังตอนใช้จริง

```python
# ✅ ถูก
train = df[df.t <  t1]
val   = df[(df.t >= t1) & (df.t < t2)]
test  = df[df.t >= t2]

# ❌ ผิด — ห้ามเด็ดขาด
train, test = train_test_split(df, shuffle=True)
```

**ตั้ง random seed ทุกที่** — numpy, torch, random · ผลต้องทำซ้ำได้

**normalize ด้วย mean/std จาก train เท่านั้น** แล้วบันทึกค่าไว้ส่งให้คน B
ถ้าคำนวณใหม่บนบอร์ด anomaly ที่ค่อยๆ ไต่จะกลายเป็น "ปกติ" ไปเอง

**เทรนด้วยข้อมูลปกติอย่างเดียว** — unsupervised · ข้อมูล anomaly ใช้แค่ตอนวัดผล

---

## ⚠️ collinearity ที่ต้องรู้ก่อนทำ Mahalanobis

| คู่ที่ไม่อิสระต่อกัน | เพราะอะไร |
|---|---|
| `eco2_ppm` ↔ `tvoc_ppb` | **eCO2 ของ ENS160 คำนวณจาก TVOC** ไม่ใช่ค่าวัดอิสระ |
| `pm1_0` ↔ `pm2_5` ↔ `pm10` | มาจากเซนเซอร์ตัวเดียว นับอนุภาคช่วงซ้อนกัน |

ทำให้เมทริกซ์ความแปรปรวนร่วมเกือบ singular → ผกผันไม่ได้หรือได้ค่าเพี้ยน
**ต้องเช็ค condition number ก่อน** และพิจารณาตัด feature หรือใช้ pseudo-inverse

รายละเอียดอยู่ใน [`../docs/05-MULTIVARIATE.md`](../docs/05-MULTIVARIATE.md)

---

## ส่งมอบให้คน B (R5 → FW-7)

ใช้ **header ภาษา C ไฟล์เดียว** แทน `.tflite` ที่วางแผนไว้ตอนแรก · `ml.baseline` สร้างให้อัตโนมัติใน `data/processed/<out>/<set>/` (ดู [`BASELINE.md`](BASELINE.md) §6)

| ไฟล์ | อยู่ที่ไหนใน header | ทำไม |
|---|---|---|
| `ag_mahal_pm_th.h` | — | ไฟล์เดียวที่ก๊อปเข้า `firmware/components/mahal/model/` · **ห้ามแก้ด้วยมือ** |
| mean / std / center ตอนเทรน | `AG_MAHAL_MEAN` · `AG_MAHAL_STD` · `AG_MAHAL_CENTER` | ต้องใช้ชุดเดียวกับตอนเทรน 🔴 ห้ามคำนวณใหม่บนบอร์ด (ยังมีอยู่ใน `norm.json` ด้วย) |
| precision matrix (Σ*⁻¹) | `AG_MAHAL_PRECISION` | Ledoit–Wolf · กลับเมทริกซ์บน PC ครั้งเดียว · ปัดเป็น float32 ก่อนให้คะแนน |
| threshold + persistence K | `AG_MAHAL_THRESHOLD_D2` · `AG_MAHAL_PERSISTENCE_K` | เกณฑ์ตัดสิน (เลือกบน val เท่านั้น) |
| ชุดฟีเจอร์ + transform | `AG_MAHAL_NUMERIC_INDEX` · `AG_MAHAL_TRANSFORM` | ลำดับตามคอลัมน์ §7 · log1p สำหรับ PM/TVOC |
| **golden vectors + persistence fixture** | `AG_MAHAL_GOLDEN_*` · `AG_MAHAL_SEQUENCE_*` | 🔑 **หลักฐานว่าผลบน PC = ผลบนบอร์ด** · บอร์ดรัน self-test ตอนบูต (`mahal self-test PASS`) · ตรวจบน PC ก่อนด้วย `python -m ml.dryrun check-header <header>` |
| `AG_MAHAL_MODEL_VERSION` | — | log ตอนบูตบอกว่าบอร์ดรันโมเดลตัวไหนอยู่ |

หลังเปลี่ยน header: `firmware/test_host/run_tests.ps1` → `idf.py build` → flash · 🔴 ตอนนี้ header ใน branch `fw/fw67-integrate` ยังเป็นข้อมูลสังเคราะห์ (`ml3-v1+pm_th+b07dd6863436`)

---

## เริ่มใช้งาน

```bash
python -m venv .venv
source .venv/bin/activate        # Windows: .venv\Scripts\activate
pip install -r requirements.txt
pytest                            # ต้องผ่านก่อนเขียนของใหม่
```

**DustBoy API** — ลงทะเบียนรอแอดมินอนุมัติหลายวัน **ทำวันนี้**
วิธีอยู่ใน [`../docs/04-DATASETS.md`](../docs/04-DATASETS.md) · สคริปต์ดึงข้อมูล `fetch_dustboy.py`
