#include "anchors_store.h"

#include <Preferences.h>

namespace {
constexpr const char* NS = "concresense";
Preferences prefs;
}  // namespace

namespace anchors {

void load(MoistureAnchors& ma, LoadCellAnchors& la) {
  prefs.begin(NS, true);  // read-only
  ma.mvDry = prefs.getFloat("m_dry", ma.mvDry);
  ma.mvSat = prefs.getFloat("m_sat", ma.mvSat);
  ma.refTempC = prefs.getFloat("m_tref", ma.refTempC);
  ma.calibrated = prefs.getBool("m_cal", false);

  la.countsPerNewton = prefs.getFloat("l_cpn", la.countsPerNewton);
  la.plungerAreaM2 = prefs.getFloat("l_area", la.plungerAreaM2);
  la.calibrated = prefs.getBool("l_cal", false);
  prefs.end();
}

void saveMoisture(const MoistureAnchors& ma) {
  prefs.begin(NS, false);
  prefs.putFloat("m_dry", ma.mvDry);
  prefs.putFloat("m_sat", ma.mvSat);
  prefs.putFloat("m_tref", ma.refTempC);
  prefs.putBool("m_cal", ma.calibrated);
  prefs.end();
}

void saveLoadCell(const LoadCellAnchors& la) {
  prefs.begin(NS, false);
  prefs.putFloat("l_cpn", la.countsPerNewton);
  prefs.putFloat("l_area", la.plungerAreaM2);
  prefs.putBool("l_cal", la.calibrated);
  prefs.end();
}

void clear() {
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
}

}  // namespace anchors
