#pragma once
// The CO2 level at which the device says "ventilate" — NVS "al_co2", chosen by the owner.
//
// One header because these numbers are now PROMISED to the owner. Until 2026-09-30 the console
// offered three fixed values and nothing else could be typed, so the band below was only a guard
// against a hand-written API call and it did not matter that it lived in two .cpp files. With a
// Custom field the owner types a number and the console has to tell them what it will accept —
// and a console promising one band while the firmware clamps to another means the owner types 650,
// is told nothing, and the device quietly stores something else.
//
// Three readers: the save handler that clamps what arrives, the cycle that reads it back, and the
// console, which is checked against this file by t_console.
#include <stdint.h>

// What the device ships with, and what the console calls "recommended". Around the common indoor
// guideline of ~1000 ppm: below it the air reads fresh, above ~1400 poor.
static constexpr uint32_t CO2_ALERT_DEFAULT = 1100;

// The band the device accepts. Not a comfort judgement — an honesty one. Outdoor air is ~420 ppm
// and the sensor's own accuracy is +-(40 ppm + 5 %), so a threshold near outdoor air would alarm on
// noise and never clear. Above 1600 the alert stops being a warning: the class-1 verdict would sit
// behind the WARNING level (>=1500 ppm) that the device raises regardless, so the owner's setting
// would have no effect they could see.
static constexpr uint32_t CO2_ALERT_MIN = 700;
static constexpr uint32_t CO2_ALERT_MAX = 1600;

static_assert(CO2_ALERT_MIN >= 500 && CO2_ALERT_MIN < CO2_ALERT_MAX,
              "floor must sit above outdoor air and below the ceiling");
static_assert(CO2_ALERT_DEFAULT >= CO2_ALERT_MIN && CO2_ALERT_DEFAULT <= CO2_ALERT_MAX,
              "the shipped level must be one the device would accept from the owner");
