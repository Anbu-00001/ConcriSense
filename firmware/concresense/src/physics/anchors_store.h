#pragma once
#include "calibration.h"

// Persistence for per-board calibration anchors, in NVS.
//
// Calibration has to survive a reboot. Without this the device silently reverts
// to placeholder constants after every power cycle and reports plausible-looking
// but meaningless w/c values -- the worst possible failure mode for an
// instrument, because nothing about the output says it is wrong.
//
// The `calibrated` flag is stored alongside the values and gates every derived
// reading, so an uncalibrated board reports "not calibrated" rather than a
// number.
namespace anchors {

void load(MoistureAnchors& ma, LoadCellAnchors& la);
void saveMoisture(const MoistureAnchors& ma);
void saveLoadCell(const LoadCellAnchors& la);
void clear();

}  // namespace anchors
