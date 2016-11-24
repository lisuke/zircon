// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/rtc.h>
#include <hw/inout.h>

#include <magenta/syscalls.h>
#include <magenta/types.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#define RTC_IO_BASE 0x70
#define RTC_NUM_IO_REGISTERS 8

#define RTC_IDX_REG 0x70
#define RTC_DATA_REG 0x71

// This is run on boot (after validation of the RTC) and whenever the
// RTC is adjusted.
static void set_utc_offset(rtc_t* rtc) {
    // Start with seconds from the Unix epoch to 2016/1/1T00:00:00.
    uint64_t seconds_to_new_year = 1451606400;

    // Leading 0 allows using the 1-indexed month values from rtc.
    static uint64_t days_in_month[] = {
        0,
        31, // January
        29, // February
        31, // March
        30, // April
        31, // May
        30, // June
        31, // July
        31, // August
        30, // September
        31, // October
        30, // November
        31, // December
    };

    // First add all the prior complete months.
    uint64_t days_this_year = 0;
    for (size_t month = 1; month < rtc->month; month++) {
        days_this_year += days_in_month[month];
    }

    // Add all the prior complete days.
    days_this_year += rtc->day - 1;

    // Hours, minutes, and seconds are 0 indexed.
    uint64_t hours_this_year = days_this_year * 24 + rtc->hours;
    uint64_t minutes_this_year = hours_this_year * 60 + rtc->minutes;
    uint64_t seconds_this_year = minutes_this_year * 60 + rtc->seconds;

    uint64_t rtc_seconds = seconds_to_new_year + seconds_this_year;
    uint64_t rtc_nanoseconds = rtc_seconds * 1000000000;

    uint64_t monotonic_nanoseconds = mx_time_get(MX_CLOCK_MONOTONIC);
    int64_t offset = rtc_nanoseconds - monotonic_nanoseconds;

    mx_status_t status = mx_clock_adjust(get_root_resource(), MX_CLOCK_UTC, offset);
    if (status != NO_ERROR) {
        fprintf(stderr, "The RTC driver was unable to set the UTC clock!\n");
    }
}

static mtx_t lock = MTX_INIT;

enum intel_rtc_registers {
    REG_SECONDS,
    REG_SECONDS_ALARM,
    REG_MINUTES,
    REG_MINUTES_ALARM,
    REG_HOURS,
    REG_HOURS_ALARM,
    REG_DAY_OF_WEEK,
    REG_DAY_OF_MONTH,
    REG_MONTH,
    REG_YEAR,
    REG_A,
    REG_B,
    REG_C,
    REG_D,
};

enum intel_rtc_register_a {
    REG_A_UPDATE_IN_PROGRESS_BIT = 1 << 7,
};

enum intel_rtc_register_b {
    REG_B_DAYLIGHT_SAVINGS_ENABLE_BIT = 1 << 0,
    REG_B_HOUR_FORMAT_BIT = 1 << 1,
    REG_B_DATA_MODE_BIT = 1 << 2,
    REG_B_SQUARE_WAVE_ENABLE_BIT = 1 << 3,
    REG_B_UPDATE_ENDED_INTERRUPT_ENABLE_BIT = 1 << 4,
    REB_B_ALARM_INTERRUPT_ENABLE_BIT = 1 << 5,
    REG_B_PERIODIC_INTERRUPT_ENABLE_BIT = 1 << 6,
    REG_B_UPDATE_CYCLE_INHIBIT_BIT = 1 << 7,
};

static uint8_t read_reg(enum intel_rtc_registers reg) {
    outp(RTC_IDX_REG, reg);
    return inp(RTC_DATA_REG);
}

static void write_reg(enum intel_rtc_registers reg, uint8_t val) {
    outp(RTC_IDX_REG, reg);
    outp(RTC_DATA_REG, val);
}

// Set the hour format to 24 hours, and the data mode to binary, rather than
// binary-coded decimal.
static void set_rtc_mode(void) {
    write_reg(REG_B, read_reg(REG_B) | REG_B_HOUR_FORMAT_BIT | REG_B_DATA_MODE_BIT);
}

static void read_time(rtc_t* rtc) {
    mtx_lock(&lock);
    set_rtc_mode();

    rtc->seconds = read_reg(REG_SECONDS);
    rtc->minutes = read_reg(REG_MINUTES);
    rtc->hours = read_reg(REG_HOURS);

    rtc->day = read_reg(REG_DAY_OF_MONTH);
    rtc->month = read_reg(REG_MONTH);
    rtc->year = read_reg(REG_YEAR) + 2000;

    mtx_unlock(&lock);
}

static void write_time(const rtc_t* rtc) {
    mtx_lock(&lock);
    set_rtc_mode();

    write_reg(REG_B, read_reg(REG_B) | REG_B_UPDATE_CYCLE_INHIBIT_BIT);

    write_reg(REG_SECONDS, rtc->seconds);
    write_reg(REG_MINUTES, rtc->minutes);
    write_reg(REG_HOURS, rtc->hours);

    write_reg(REG_DAY_OF_MONTH, rtc->day);
    write_reg(REG_MONTH, rtc->month);
    write_reg(REG_YEAR, rtc->year - 2000);

    write_reg(REG_B, read_reg(REG_B) & ~REG_B_UPDATE_CYCLE_INHIBIT_BIT);

    mtx_unlock(&lock);
}

static ssize_t intel_rtc_get(void* buf, size_t count) {
    if (count < sizeof(rtc_t)) {
        return ERR_BUFFER_TOO_SMALL;
    }

    // Ensure we have a consistent time.
    rtc_t rtc, prev;
    do {
        // Using memcpy, as we use memcmp to compare.
        memcpy(&prev, &rtc, sizeof(rtc_t));
        read_time(&rtc);
    } while (memcmp(&rtc, &prev, sizeof(rtc_t)));

    memcpy(buf, &rtc, sizeof(rtc_t));
    return sizeof(rtc_t);
}

static bool rtc_is_invalid(const rtc_t* rtc) {
    return rtc->seconds > 59 ||
        rtc->minutes > 59 ||
        rtc->hours > 23 ||
        rtc->day > 31 ||
        rtc->month > 12 ||
        rtc->year < 2000 ||
        rtc->year > 2099;
}

static ssize_t intel_rtc_set(const void* buf, size_t count) {
    if (count < sizeof(rtc_t)) {
        return ERR_BUFFER_TOO_SMALL;
    }
    rtc_t rtc;
    memcpy(&rtc, buf, sizeof(rtc_t));

    // An invalid time was supplied.
    if (rtc_is_invalid(&rtc)) {
        return ERR_OUT_OF_RANGE;
    }

    write_time(&rtc);
    // TODO(kulakowski) This isn't the place for this long term.
    set_utc_offset(&rtc);
    return sizeof(rtc_t);
}

// Validate that the RTC is set to a valid time, and to a relatively
// sane one. Report the validated or reset time back via rtc.
static void sanitize_rtc(rtc_t* rtc) {
    // January 1, 2016 00:00:00
    static const rtc_t default_rtc = {
        .day = 1,
        .month = 1,
        .year = 2016,
    };

    intel_rtc_get(rtc, sizeof(*rtc));
    if (rtc_is_invalid(rtc) || rtc->year < 2010 || rtc->year > 2020) {
        intel_rtc_set(&default_rtc, sizeof(&default_rtc));
        *rtc = default_rtc;
    }
}

// Implement ioctl protocol.
static ssize_t intel_rtc_ioctl(mx_device_t* dev, uint32_t op,
                               const void* in_buf, size_t in_len,
                               void* out_buf, size_t out_len) {
    switch (op) {
    case IOCTL_RTC_GET:
        return intel_rtc_get(out_buf, out_len);

    case IOCTL_RTC_SET:
        return intel_rtc_set(in_buf, in_len);
    }
    return ERR_NOT_SUPPORTED;
}

static mx_protocol_device_t intel_rtc_device_proto __UNUSED = {
    .ioctl = intel_rtc_ioctl,
};

// Implement driver object.
static mx_status_t intel_rtc_init(mx_driver_t* drv) {
#if defined(__x86_64__) || defined(__i386__)
    // TODO(teisenbe): This should be probed via the ACPI pseudo bus whenever it
    // exists.

    mx_status_t status = mx_mmap_device_io(get_root_resource(), RTC_IO_BASE, RTC_NUM_IO_REGISTERS);
    if (status != NO_ERROR) {
        return status;
    }

    mx_device_t* dev;
    status = device_create(&dev, drv, "rtc", &intel_rtc_device_proto);
    if (status != NO_ERROR) {
        return status;
    }

    status = device_add(dev, driver_get_misc_device());
    if (status != NO_ERROR) {
        free(dev);
        return status;
    }

    rtc_t rtc;
    sanitize_rtc(&rtc);
    set_utc_offset(&rtc);

    return NO_ERROR;
#else
    return ERR_NOT_SUPPORTED;
#endif
}

mx_driver_t _driver_intel_rtc = {
    .ops = {
        .init = intel_rtc_init,
    },
};

MAGENTA_DRIVER_BEGIN(_driver_intel_rtc, "intel-rtc", "magenta", "0.1", 0)
MAGENTA_DRIVER_END(_driver_intel_rtc)
