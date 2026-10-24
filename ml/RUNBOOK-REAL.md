# RUNBOOK — ML บนข้อมูลจริง (PowerShell · รันจากโฟลเดอร์ราก repo)

> R2 · 24 ต.ค. 2569 · ซ้อมครบทั้งสายแล้วด้วยข้อมูลสังเคราะห์ (§6) · **ยังไม่เคยรันกับข้อมูลจริง**
> โปรโตคอลการเก็บ/เหตุการณ์: `data/PROTOCOL-TRAIN-2026-10.md` · กฎ: `CLAUDE.md` §2

---

## 0. ตั้งค่าทุกครั้งที่เปิด terminal ใหม่

```powershell
.venv\Scripts\Activate.ps1
$env:PYTHONIOENCODING = "utf-8"     # กัน synth.py / baseline.py crash ตอนพิมพ์ภาษาไทย
pytest ml/ -q                         # ต้องผ่านก่อนเริ่ม
```

- ต้องการรูปจาก `ml.qa` → ติดตั้ง matplotlib ก่อน (`pip install -r ml/requirements.txt`) · ไม่มีก็รันได้ แค่ข้ามรูป
- 🔴 **`ml.baseline --raw` ไม่ขยาย `*` เอง** (PowerShell ก็ไม่ขยายให้) → ส่งรายชื่อไฟล์ด้วย `(Get-ChildItem ...).FullName` ตาม §4
  · `ml.qa` และ `ml.events` ขยายเองได้ ใส่ glob ในเครื่องหมายคำพูด

---

## 1. ทุกเช้าระหว่างเก็บ — QA

```powershell
python -m ml.qa --raw "data/raw/usb_train/AG_*_usb.csv" --out data/processed/qa/2026-10-25
```

| ผล | ทำอะไร |
|---|---|
| `PASS` | ไม่ต้องทำอะไร |
| `WARN` | อ่านเหตุผลใน `qa.md` · รีบูต / ค่าค้าง / marked ที่ยังไม่มีชื่อ → จดลงรายงานประจำวัน |
| `FAIL` (exit 1) | ตรวจหน้างานทันที: logger ยังรันไหม · โน้ตบุ๊กพักไหม · สาย USB · **ห้าม flash** |
| exit 2 | อ่านไฟล์ไม่ได้ ข้อความบอกชื่อไฟล์ · เช่น header ผิด หรือมีไฟล์ SD ปนกับ USB (timestamp ซ้ำ) |

---

## 2. หลังทำเหตุการณ์ — ไฟล์ events

```powershell
# 2.1 ร่างจาก MARK (end + 1 s ให้อัตโนมัติ · label = TODO) — ไม่เขียนทับไฟล์ที่มีอยู่
python -m ml.events draft --ev "data/raw/usb_train/EV_*_usb.csv" --out data/events/2026-10_train.csv

# 2.2 แก้ด้วยมือ: TODO → incense / dusting / cooking / other · เติม note จากใบจด
#     เพิ่มแถว cooking ที่จดด้วยมือในช่วง train (โปรโตคอล §3 ทาง ก)

# 2.3 ตรวจ — ต้องได้ OK (exit 0)
python -m ml.events check --events data/events/2026-10_train.csv `
  --raw "data/raw/usb_train/AG_*_usb.csv" --ev "data/raw/usb_train/EV_*_usb.csv" `
  --t1 <t1> --t2 <t2>
```

- WARN "ไม่มี MARK คู่" = แถวที่จดมือ → ปกติสำหรับ cooking ในช่วง train
- ผ่านแล้ว **commit `data/events/2026-10_train.csv` ทันที** (ground truth สร้างใหม่ไม่ได้)

---

## 3. R3 — freeze ก่อนเริ่มเหตุการณ์ val

ยังไม่มีสคริปต์ (R3) · ค่าที่ต้อง freeze และจดลงรายงาน R3: ชุด feature (`pm_th`) · `t1` · `gap_s` (60) · ช่วงที่ตัดทิ้ง (ถ้ามี)

---

## 4. R4 — รันจริงครั้งเดียว

```powershell
$raw = (Get-ChildItem data\raw\usb_train\AG_*_usb.csv).FullName
python -m ml.baseline --raw $raw `
  --events data/events/2026-10_train.csv `
  --mapping "incense=anomaly,dusting=anomaly,cooking=normal,other=unknown" `
  --row-support-s 1 --t1 <t1> --t2 <t2> --gap-s 60 `
  --feature-set pm_th --seed 42 --out data/processed/ml-real
```

- threshold และ k ถูกเลือกบน val แล้ว hash (`frozen_sha256`) **ภายในโค้ดก่อน**ให้คะแนน test · หน้าจอพิมพ์ผล test ตอนท้ายของคำสั่งเดียวกัน
- 🔴 **รันแล้วห้ามเปลี่ยนค่าใดๆ แล้วรันใหม่** · ถ้าเจ้าของสั่งให้รันใหม่ ต้องเขียนในรายงานว่า test ถูกใช้มากกว่า 1 ครั้ง พร้อมผลทุกครั้ง
- จด `version=` (บรรทัดที่พิมพ์ออกมา) และ `frozen_sha256`:
  ```powershell
  (Get-Content data\processed\ml-real\pm_th\model.json -Raw | ConvertFrom-Json).frozen_sha256
  ```
- ไฟล์ผล: `data/processed/ml-real/pm_th/` → `model.json` · `norm.json` · `results.json` · `ag_mahal_pm_th.h` · `test-mahalanobis/`

---

## 5. ตรวจ header ก่อนส่ง FW-7

```powershell
python -m ml.dryrun check-header data\processed\ml-real\pm_th\ag_mahal_pm_th.h    # ต้องได้ PASS: bad 0
```

ใช้ gcc บน PC (x86-64) · ผลบน ESP32-S3 ต้องวัดใน FW-7 ขั้น 3 (R5)

---

## 6. ซ้อม (ข้อมูลสังเคราะห์ · สมมติฐาน ไม่ใช่ค่าวัดจริง)

```powershell
Remove-Item -Recurse data\processed\dryrun -ErrorAction SilentlyContinue   # make ไม่เขียนทับ
python -m ml.dryrun make --days 5 --seed 42 --out data/processed/dryrun
python -m ml.qa --raw "data/processed/dryrun/usb/AG_*_usb.csv" --out data/processed/dryrun/qa
python -m ml.events draft --ev "data/processed/dryrun/usb/EV_*_usb.csv" --out data/processed/dryrun/events_draft.csv
python -m ml.dryrun fill --draft data/processed/dryrun/events_draft.csv `
  --truth data/processed/dryrun/truth_events.csv --out data/processed/dryrun/events.csv
python -m ml.events check --events data/processed/dryrun/events.csv `
  --raw "data/processed/dryrun/usb/AG_*_usb.csv" --ev "data/processed/dryrun/usb/EV_*_usb.csv" `
  --allowed spike,drift,decorrelation --t1 2026-09-26T00:00:00+07:00 --t2 2026-09-27T00:00:00+07:00
$raw = (Get-ChildItem data\processed\dryrun\usb\AG_*_usb.csv).FullName
python -m ml.baseline --raw $raw --events data/processed/dryrun/events.csv `
  --mapping "spike=anomaly,drift=anomaly,decorrelation=anomaly" --row-support-s 1 `
  --t1 2026-09-26T00:00:00+07:00 --t2 2026-09-27T00:00:00+07:00 --feature-set pm_th --seed 42 `
  --out data/processed/dryrun/ml
python -m ml.dryrun check-header data\processed\dryrun\ml\pm_th\ag_mahal_pm_th.h
```

- 🔴 **header จากการซ้อมห้ามเอาไปใส่เฟิร์มแวร์** — baseline พิมพ์ `real data` และ `data_source = real` เพราะมาจาก `--raw` ทั้งที่เป็นข้อมูลสังเคราะห์
- ฉบับย่อที่รันใน pytest: `ml/tests/test_dryrun.py` (4 วัน · ประมาณ 30 s)
