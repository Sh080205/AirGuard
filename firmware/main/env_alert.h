#ifndef AIRGUARD_ENV_ALERT_H
#define AIRGUARD_ENV_ALERT_H

/* demo 5 พ.ย. 2569 (เจ้าของสั่ง): ไฟ/เสียง WATCH/ALARM มาจากเกณฑ์ PM2.5 และ RH
 * แยกกันแทนผล ML · ML (Mahalanobis) ยังคำนวณ ยังลง AGML และยังแสดง D² บน
 * dashboard/OLED ตามเดิม แต่ไม่คุม LED/buzzer แล้ว
 * เหตุผล: โมเดลเทรนที่ RH 57 ± 3 % ห้องที่แห้งกว่าจึงขึ้น ML ALARM ค้าง
 * (docs/bringup/2026-11-03.md §6)
 *
 * เกณฑ์ (เจ้าของเลือก 5 พ.ย. · นโยบายเฟิร์มแวร์ ไม่ใช่เกณฑ์ทางการแพทย์):
 *   PM2.5 >= 37.5 µg/m³  -> WATCH   (37.5 = ตัวเลขมาตรฐานไทยของค่าเฉลี่ย 24 ชม.
 *                                     ที่นี่ใช้กับค่ารายวินาที)
 *   PM2.5 >= 75 µg/m³    -> ALARM   (2 เท่าของ 37.5)
 *   RH < 30 % หรือ > 70 % -> WATCH   (RH ไม่ขึ้น ALARM)
 * ⚠️ RH วัดจาก AHT21 ข้างฮีตเตอร์ ENS160 จึงเป็นค่ารอบโมดูล ไม่ใช่ค่าห้อง
 *
 * เงื่อนไขต้องเป็นจริงติดกัน ENV_RAISE_ROWS แถว (1 Hz) ก่อนขึ้นระดับ เพื่อ
 * ไม่ให้ค่าแหลมวินาทีเดียวทำให้เตือน (ค่าคาดหมาย ต้องยืนยันด้วยการวัด) ·
 * การลดระดับใช้กติกาเดิมของ ag_fsm: ต่ำกว่าเกณฑ์ติดกัน 30 s ลดทีละระดับ
 *
 * วิธีต่อเข้า FSM: แปลงผลเป็น ag_fsm_ml_t ที่ FSM รู้จักอยู่แล้ว
 * (PERSISTENT/ABOVE/BELOW/MISSING) ไม่ต้องแก้ตรรกะใน ag_fsm.c
 *
 * 🔴 เส้นทาง CO ไม่ผ่านโมดูลนี้ (CLAUDE.md §2) — io_compose.h ยังให้ CO ชนะเสมอ
 *
 * Pure C header-only: ไม่มี FreeRTOS/GPIO/float · เจ้าของคือ io task คนเดียว */

#include <stdbool.h>
#include <stdint.h>

#include "ag_fsm.h"

#define ENV_PM_WATCH_X10   375U    /* 37.5 µg/m³ */
#define ENV_PM_ALARM_X10   750U    /* 75 µg/m³ */
#define ENV_RH_LOW_X10     300     /* < 30.0 %RH */
#define ENV_RH_HIGH_X10    700     /* > 70.0 %RH */
#define ENV_RAISE_ROWS     5U

/* เหตุผลที่เตือน (บิต) */
#define ENV_R_PM_WATCH     (1U << 0)
#define ENV_R_PM_ALARM     (1U << 1)
#define ENV_R_RH_LOW       (1U << 2)
#define ENV_R_RH_HIGH      (1U << 3)
#define ENV_R_COUNT        4U

typedef struct {
    bool seen;
    int64_t last_at_us;
    uint8_t count[ENV_R_COUNT];   /* แถวติดกันที่เงื่อนไขแต่ละข้อเป็นจริง */
    uint8_t raw;                  /* เงื่อนไขที่จริงในแถวล่าสุด */
    uint8_t reasons;              /* เงื่อนไขที่จริงติดกัน >= ENV_RAISE_ROWS แถว */
    bool any_data;                /* แถวล่าสุดมี PM หรือ RH */
} env_alert_t;

/* หนึ่งแถว · has_pm/has_rh = false คือคอลัมน์ว่าง (ไม่นับว่าเกินหรือไม่เกิน) */
static inline uint8_t env_alert_raw(bool has_pm, uint16_t pm2_5, bool has_rh, int32_t rh_x10)
{
    uint8_t r = 0;
    if (has_pm) {
        const uint32_t pm_x10 = (uint32_t)pm2_5 * 10U;
        if (pm_x10 >= ENV_PM_WATCH_X10) {
            r |= ENV_R_PM_WATCH;
        }
        if (pm_x10 >= ENV_PM_ALARM_X10) {
            r |= ENV_R_PM_ALARM;
        }
    }
    if (has_rh) {
        if (rh_x10 < ENV_RH_LOW_X10) {
            r |= ENV_R_RH_LOW;
        }
        if (rh_x10 > ENV_RH_HIGH_X10) {
            r |= ENV_R_RH_HIGH;
        }
    }
    return r;
}

static inline ag_fsm_level_t env_alert_level_of(uint8_t reasons)
{
    if ((reasons & ENV_R_PM_ALARM) != 0U) {
        return AG_FSM_LEVEL_ALARM;
    }
    return reasons != 0U ? AG_FSM_LEVEL_WATCH : AG_FSM_LEVEL_NONE;
}

/* ป้อนแถวใหม่ (at_us ต่างจากครั้งก่อน = แถวใหม่ · แถวเดิมซ้ำไม่นับซ้ำ) */
static inline void env_alert_row(env_alert_t *e, int64_t at_us, bool has_pm, uint16_t pm2_5,
                                 bool has_rh, int32_t rh_x10)
{
    if (e->seen && at_us == e->last_at_us) {
        return;
    }
    e->seen = true;
    e->last_at_us = at_us;
    e->any_data = has_pm || has_rh;
    e->raw = env_alert_raw(has_pm, pm2_5, has_rh, rh_x10);
    e->reasons = 0;
    for (unsigned i = 0; i < ENV_R_COUNT; ++i) {
        const uint8_t bit = (uint8_t)(1U << i);
        /* ไม่เกิน หรือคอลัมน์ว่าง = นับใหม่ */
        if ((e->raw & bit) != 0U) {
            if (e->count[i] < UINT8_MAX) {
                e->count[i]++;
            }
        } else {
            e->count[i] = 0;
        }
        if (e->count[i] >= ENV_RAISE_ROWS) {
            e->reasons |= bit;
        }
    }
}

/* แปลงเป็นอินพุต ML ของ FSM · current = ระดับที่ FSM ถืออยู่ (ml_level ของ
 * step ก่อน) เพื่อให้ ALARM ลดเหลือ WATCH ได้เมื่อ PM ลงมาอยู่ช่วง WATCH
 *   ระดับที่ยืนยันแล้ว > NONE และ >= current -> PERSISTENT (FSM ขึ้น/คงระดับ)
 *   ยังเกินระดับ current อยู่ หรือกำลังนับขึ้น  -> ABOVE (FSM ไม่เริ่มนับลด)
 *   แถวล่าสุดไม่มีทั้ง PM และ RH              -> MISSING
 *   อื่นๆ                                     -> BELOW (FSM ลดทีละระดับทุก 30 s) */
static inline ag_fsm_ml_t env_alert_fsm_status(const env_alert_t *e, ag_fsm_level_t current,
                                               uint16_t *streak)
{
    *streak = 0;
    if (!e->seen) {
        return AG_FSM_ML_NONE;
    }
    if (!e->any_data) {
        return AG_FSM_ML_MISSING;
    }
    const ag_fsm_level_t confirmed = env_alert_level_of(e->reasons);
    if (confirmed != AG_FSM_LEVEL_NONE && confirmed >= current) {
        *streak = confirmed == AG_FSM_LEVEL_ALARM ? (uint16_t)AG_FSM_ML_ALARM_STREAK : 1U;
        return AG_FSM_ML_PERSISTENT;
    }
    const ag_fsm_level_t raw = env_alert_level_of(e->raw);
    if (raw != AG_FSM_LEVEL_NONE && raw >= current) {
        return AG_FSM_ML_ABOVE;
    }
    return AG_FSM_ML_BELOW;
}

#endif
