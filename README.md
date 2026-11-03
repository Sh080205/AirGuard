# AirGuard-S3

**เครื่องเฝ้าระวังคุณภาพอากาศในห้อง + ตรวจจับความผิดปกติบนอุปกรณ์ สำหรับผู้ป่วยภูมิแพ้และโรคทางเดินหายใจ**

ESP32-S3 · TinyML · บอร์ดออกแบบเอง
วิชา 03603323 / 03603324 · วิศวกรรมคอมพิวเตอร์ ม.เกษตรศาสตร์ ศรีราชา

---

## โปรเจกต์นี้ต่างจากเครื่องวัดอากาศทั่วไปยังไง

เครื่องที่มีขายเตือนเมื่อค่าเกินเกณฑ์ แต่**สิ่งที่กระตุ้นอาการหลายครั้งไม่ได้ทำให้ค่าไหนเกินเกณฑ์เลย**

```
ทำอาหาร       PM2.5 ↑↑   อุณหภูมิ ↑   ความชื้น ↑    →  ขึ้นด้วยกัน มีเหตุผล  =  ปกติ
ปัดฝุ่น / ราฟุ้ง   PM2.5 ↑↑   อุณหภูมิ →   ความชื้น →    →  ฝุ่นขึ้นลอยๆ         =  ผิดปกติ
```

ทั้งสองกรณี PM2.5 อ่านได้เท่ากันได้ — **เกณฑ์ตัวเลขแยกไม่ออก แต่ multivariate แยกออก**

เราจำลองไว้แล้ว: **3σ แบบดูทีละตัวจับ anomaly ประเภทนี้ได้ 0% · Mahalanobis จับได้ 100%**
นี่คือเหตุผลทั้งหมดที่ต้องมี autoencoder แทน `if (pm25 > 50)`

---

## สถานะ

> อัปเดต 1 พ.ย. 2569 จาก git (`main` @ `7e926b4`) · รายละเอียดและผลวัดอยู่ใน [`CLAUDE.md`](CLAUDE.md) §10 — ถ้าขัดกัน ให้ยึด §10

| ส่วน | สถานะ |
|---|---|
| Schematic | ✅ เสร็จ · ตรวจกับ netlist ครบ 28 จุด 37 เน็ต |
| PCB + PCBA | ✅ มาถึงแล้ว · DevKit เสียบซ็อกเก็ตได้พอดี · บัดกรี J1 · C1 · BZ1 แล้ว · J7 ว่าง · 🔴 C13 ไม่ใส่ |
| ขึ้นบอร์ด | 🟡 ขั้น 1–8 ผ่าน (13–14 ต.ค.) **ยกเว้นการเขียน SD** (ช่องการ์ดของโมดูลใช้ไม่ได้ → บันทึกผ่าน USB แทน) · ENS160 (16–17 ต.ค.) · ZE07-CO (21–23 ต.ค.) — ดู [`docs/bringup/`](docs/bringup/) |
| ตรวจกับ datasheet | ✅ ตารางขา · LDO · เซนเซอร์ · ระยะขา DevKit |
| เฟิร์มแวร์ | ✅ merge แล้ว: FW-1–4 (logger + serial CSV mirror) · FW-5a (ENS160) · FW-5b (ZE07-CO + เส้นทาง CO แยกจาก ML) — ทดสอบบนบอร์ดแล้ว · ⬜ ALARM ของ CO ทดสอบด้วย host test เท่านั้น<br>🟡 FW-6 (FSM) + FW-7 (Mahalanobis บนบอร์ด) อยู่ใน branch `fw/fw67-integrate` ยังไม่ merge · build + host test ผ่าน · **ยังไม่ทดสอบบนบอร์ด** · โมเดลยังเป็นข้อมูลสังเคราะห์<br>⬜ เฟิร์มแวร์จอ OLED ยังไม่มี |
| ML pipeline | ✅ ML-1 (synth/load/split) · ML-2 (metrics) · ML-3 (3σ vs Mahalanobis + export header) · R0 (`ml.qa`) · R1 (`ml.events`) · R2 (`ml.dryrun`) merge แล้ว<br>⬜ R3–R6 (เทรนและวัดผลบนข้อมูลจริง → header ให้บอร์ด → รูปสำหรับสไลด์) รอข้อมูล · autoencoder ยังไม่มีโค้ด |
| เก็บข้อมูลจริง | 🟡 เริ่มเก็บ 25 ต.ค. 20:24 ผ่าน USB (นอกกล่อง) ตามโปรโตคอล [`data/PROTOCOL-TRAIN-2026-10.md`](data/PROTOCOL-TRAIN-2026-10.md) · ตามแผนจบ 30 ต.ค. 22:00 · ไฟล์ raw ไม่ขึ้น git (ตั้งใจ) · ⬜ ไฟล์ events (`data/events/`) ยังไม่ได้ push · ใบจดเหตุการณ์ val/test ใน repo ยังว่าง |
| กล่อง | 🟡 รุ่น 2 ([`hardware/enclosure_v2/`](hardware/enclosure_v2/)) export STL แล้ว · ส่งร้านพิมพ์ 24 ต.ค. · ใน repo ยังไม่มีบันทึกว่าได้รับชิ้นงาน |

---

## โครงสร้าง

```
CLAUDE.md          บริบทถาวร — GPIO · กฎเหล็ก · สัญญารูปแบบข้อมูล
docs/              เอกสารทั้งหมด
firmware/          คน B — ESP-IDF v5.5.x
ml/                คน C — Python
data/              ข้อมูลที่เก็บได้ (raw ไม่ commit)
```

### เอกสารสำคัญ

| ไฟล์ | เนื้อหา |
|---|---|
| [`docs/11-CODE-MAP.md`](docs/11-CODE-MAP.md) | 🗺️ **แผนที่โค้ด + คำอธิบายโค้ด** — task · queue · หนึ่งรอบของ sampler · เส้นทาง CO · ML บนบอร์ด · ไฟล์ firmware/ml ทีละไฟล์ |
| [`docs/02-HARDWARE_REFERENCE.md`](docs/02-HARDWARE_REFERENCE.md) | 📖 ตารางขา · pinout · ค่าอุปกรณ์ · ตัวเลขสำหรับ lab report |
| [`docs/08-BRINGUP.md`](docs/08-BRINGUP.md) | ขึ้นบอร์ด · build / flash · ตารางทดสอบ |
| [`docs/03-CO_SAFETY.md`](docs/03-CO_SAFETY.md) | 🔴 เกณฑ์ CO · ทำไมไม่ใช่ CO alarm |
| [`docs/04-DATASETS.md`](docs/04-DATASETS.md) | DustBoy · Air4Thai · OpenAQ |
| [`docs/05-MULTIVARIATE.md`](docs/05-MULTIVARIATE.md) | Mahalanobis · χ² · collinearity · PCA vs autoencoder |
| [`docs/06-PRIOR_ART.md`](docs/06-PRIOR_ART.md) | งานที่คนอื่นทำแล้ว + ช่องว่างที่เราเติม |
| [`docs/07-GIT_WORKFLOW.md`](docs/07-GIT_WORKFLOW.md) | ⚙️ วิธีขึ้น GitHub · กฎ branch · สิ่งที่ห้าม commit |
| [`docs/diagrams.html`](docs/diagrams.html) | ไดอะแกรม 5 แผ่น — การต่อโมดูล · ไฟเลี้ยง · data flow · FSM · บรรจุภัณฑ์ |
| [`hardware/frame/`](hardware/frame/) | กล่อง OpenSCAD + viewer · `docs/archive/` = เอกสารเก่า ห้ามใช้อ้างอิง |

---

## ฮาร์ดแวร์ย่อ

| | |
|---|---|
| MCU | ESP32-S3-DevKitC-1 (N16R8) บนซ็อกเก็ต 2×22 |
| ฝุ่น | PMS7003 — PM1.0 / PM2.5 / PM10 · UART |
| ก๊าซ + อุณหภูมิ/ความชื้น | โมดูลคอมโบ ENS160 (TVOC, eCO2) + AHT21 (T/RH) · I²C |
| CO | ZE07-CO · UART · 🔴 แยก safety path จาก ML |
| CO₂ | SCD40 NDIR *(ตัวเลือก)* · I²C |
| เก็บข้อมูล | microSD · SPI |
| แจ้งเตือน | LED ×3 · buzzer ผ่าน MOSFET |
| รับคำสั่ง | ปุ่ม MARK / MODE / ACK |

### 🔴 3 อย่างที่ผิดแล้วเจ็บ

1. **ห้ามใช้ GPIO 35/36/37** (PSRAM) · **0/3/45/46** (strapping) · **38/48** (RGB LED)
2. **ห้ามใส่ C13** — 470 µF เกินพิกัด COUT ของ LDO (1–10 µF) ถึง 47 เท่า
3. **ซื้อ DevKit ต้องถามความกว้างบอร์ด** — **25.4 mm = ใช่** · 28 mm = เสียบไม่ได้

---

## ตั้งค่าเครื่องพัฒนา

**เฟิร์มแวร์**
```bash
# ต้องมี ESP-IDF v5.5.x
cd firmware
idf.py set-target esp32s3
idf.py menuconfig          # Flash 16MB · PSRAM Octal 8MB
idf.py build
idf.py -p <PORT> flash monitor
```

**ML**
```bash
cd ml
python -m venv .venv && source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt
pytest                      # ต้องผ่านก่อนเริ่มเขียนของใหม่
```

---

## ⚠️ ข้อจำกัดการใช้งาน

> **อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
> และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์
> ห้ามใช้แทน CO alarm ที่ได้มาตรฐาน UL 2034 หรือ EN 50291**

ผู้ผลิต ZE07-CO ระบุใน datasheet เองว่า
*"Please do not use the modules in systems which related to human being's safety."*
