> ⚠️ **ส่วน push/PR สำหรับคนเท่านั้น — AI agent ให้ทำตาม AGENTS.md**

# วิธีขึ้น GitHub และทำงานร่วมกัน 3 คน

---

## ครั้งแรก — คนใดคนหนึ่งทำคนเดียว

**1. สร้าง repo บน GitHub** — ตั้งเป็น **Private** ก่อน *(เปลี่ยนเป็น Public ตอนส่งงานได้)*
ไม่ต้องติ๊ก "Add README" หรือ "Add .gitignore" เพราะเรามีแล้ว

**2. push ขึ้นไป**

```bash
cd airguard-s3
git init
git add .
git commit -m "เริ่มโปรเจกต์: เอกสาร + โครง repo + CLAUDE.md"
git branch -M main
git remote add origin https://github.com/<ชื่อคุณ>/airguard-s3.git
git push -u origin main
```

**3. เชิญเพื่อนอีก 2 คน**
Settings → Collaborators → Add people

---

## อีก 2 คนทำครั้งเดียว

```bash
git clone https://github.com/<ชื่อเจ้าของ>/airguard-s3.git
cd airguard-s3
```

---

## 🔴 กฎเดียวที่ต้องตกลงกันให้ได้

**ห้าม push ขึ้น `main` ตรงๆ** — แยก branch ตามคน แล้วค่อยรวม

```
main          ← ของที่ใช้ได้จริง ผ่านการรีวิวแล้ว
├── fw/*      ← คน B
├── ml/*      ← คน C
└── hw/*      ← คน A
```

**ทำไมสำคัญกับทีมนี้เป็นพิเศษ** — คน B กับ C ทำงานคนละโฟลเดอร์แทบไม่ชนกัน
แต่ `CLAUDE.md` กับ `docs/` เป็นของกลาง **ถ้าสองคนแก้พร้อมกันบน main จะชนกันแน่**

---

## รอบการทำงานปกติ

```bash
git checkout main
git pull                          # 🔑 ดึงของใหม่ก่อนเสมอ

git checkout -b ml/metrics        # ตั้งชื่อตามสิ่งที่ทำ
# ...เขียนโค้ด...
git add ml/metrics.py ml/tests/test_metrics.py
git commit -m "เพิ่ม metrics: PR-AUC, event-level, MTTD, false alarm/วัน"
git push -u origin ml/metrics
```

แล้วเปิด **Pull Request** บน GitHub → ให้เพื่อนอีกคนกด Approve → Merge

> 🔑 **ให้คนอื่นรีวิวแม้จะมั่นใจ** — ไม่ใช่เพราะกลัวผิด แต่เพราะตอนนำเสนอ
> อาจารย์จะถามข้ามส่วน **คนที่ไม่เคยเห็นโค้ดอีกสองส่วนเลยจะตอบไม่ได้**

---

## ข้อความ commit

เขียนเป็น**ภาษาอังกฤษ** รูปคำสั่ง (imperative) บอกว่า**ทำอะไร** ไม่ใช่แค่ว่าแก้ไฟล์ไหน

```
✅ Add PMS7003 frame parser with checksum-failure tests
✅ Normalize features using train-split mean/std only
✅ Update pin table after checking Espressif datasheet

❌ update
❌ fix bug
❌ edit files
```

commit เฉพาะงานจริง — ไฟล์โจทย์/บันทึกสำหรับ AI (`docs/tasks/`) เก็บไว้ในเครื่อง ไม่ขึ้น git

---

## 🔴 สิ่งที่ห้าม commit เด็ดขาด

`.gitignore` กันไว้ให้แล้วเกือบหมด แต่ต้องระวังเอง 2 อย่าง

**API key และรหัสผ่าน** — DustBoy API key, WiFi SSID/password
ใส่ในไฟล์ `.env` หรือ `secrets.h` ที่ถูก ignore ไว้

```c
// ❌ ห้ามเขียนแบบนี้ลงไฟล์ที่ commit
#define WIFI_PASS "mypassword123"

// ✅ แยกไว้ใน secrets.h ที่ .gitignore กันไว้
#include "secrets.h"
```

> ⚠️ **ถ้าเผลอ commit รหัสผ่านไปแล้ว การลบใน commit ถัดไปไม่พอ** —
> มันยังอยู่ในประวัติ git ต้องเปลี่ยนรหัสผ่านนั้นทิ้งเลย

**ข้อมูลดิบขนาดใหญ่** — CSV 1 แถว/วินาที = ~86,400 แถว/วัน
เก็บต่อเนื่อง 1 เดือนก็หลายร้อย MB · GitHub จำกัดไฟล์ละ 100 MB
→ `data/raw/` ถูก ignore ไว้แล้ว **แชร์กันผ่าน Google Drive แทน**

แต่ `data/events/` **ต้องขึ้น git** เพราะเป็น ground truth ที่สร้างใหม่ไม่ได้

---

## ถ้าชนกัน (merge conflict)

เกิดบ่อยที่สุดกับ `CLAUDE.md` และ `docs/` เพราะเป็นของกลาง

```bash
git checkout main && git pull
git checkout <branch-ของคุณ>
git merge main
# แก้ตรงที่มี <<<<<<< ======= >>>>>>>
git add <ไฟล์ที่แก้>
git commit
```

> 🔑 **ถ้าไม่แน่ใจว่าควรเก็บฝั่งไหน อย่าเดา — ถามเจ้าของอีกฝั่งก่อน**
> โดยเฉพาะใน `CLAUDE.md` เพราะตัวเลขในนั้นผ่านการตรวจกับ datasheet มาแล้ว

---

## CI ที่ตั้งไว้ให้แล้ว

ทุกครั้งที่ push หรือเปิด PR `.github/workflows/ci.yml` จะรัน

| งาน | ทำอะไร |
|---|---|
| **ML** | ติดตั้ง dependencies → `ruff check` → `pytest` |
| **Firmware** | บิลด์ด้วย ESP-IDF v5.5 target esp32s3 |
| **Docs** | เตือนถ้ามีลิงก์ภายในชี้ไปไฟล์ที่ไม่มีอยู่ |

ตอนนี้ตั้ง `continue-on-error: true` ไว้ทุกงาน **เพราะยังไม่มีโค้ด** —
🔑 **พอเขียน test ชุดแรกเสร็จแล้วให้ลบบรรทัดนั้นออก** ไม่งั้น CI จะเขียวตลอดแม้ test พัง

---

## ถ้าใช้ Claude Code

`CLAUDE.md` อยู่ที่ราก repo แล้ว → Claude Code จะอ่านเองอัตโนมัติ
**ไม่ต้องอธิบาย GPIO หรือกฎโปรเจกต์ซ้ำทุกครั้ง**

พอมีอะไรที่ยืนยันแล้วเพิ่ม *(เช่นค่าที่วัดได้จริงบนบอร์ด)* **ให้อัปเดตลง `CLAUDE.md`**
เพื่อให้ทุก session ต่อไปรู้ด้วย — ไม่ใช่เก็บไว้ในหัวคนเดียว
