'use strict';
/**
 * IS 456:2000 site audit report generator — Phase 5.
 *
 * Uses PDFKit (programmatic) rather than Puppeteer/Playwright HTML-to-PDF.
 * Checked rather than assumed: Puppeteer needs a 150-400MB Chromium binary and
 * ~150-200MB RAM per browser instance, versus PDFKit's ~50-80ms and ~10MB with
 * no system dependency at all. jsPDF was ruled out because it depends on
 * html2canvas and therefore cannot run server-side in Node.
 *
 * HONESTY: this report documents what the DEVICE measured. It carries the
 * provenance banner on every page, because the underlying classifier is trained
 * on physics-simulated data and the device has never been validated against
 * laboratory-tested concrete. A compliance-looking PDF is exactly the artifact
 * where that caveat matters most -- it is the thing someone might file.
 */

const PDFKit = require('pdfkit');

const COLORS = {
  GOOD: '#2f8f3f',
  MARGINAL: '#b5820f',
  REJECT: '#c33c33',
  UNKNOWN: '#7a7a7a',
  ink: '#1a1a1a',
  muted: '#666666',
  rule: '#cccccc',
};

// IS 456:2000-derived acceptance windows. Single source for the PDF's
// "specified limit" column so the document cannot drift from the rule engine
// in firmware/concresense/src/physics/calibration.cpp.
const LIMITS = [
  { key: 'estimated_wc_ratio', label: 'Water-cement ratio', unit: '',
    good: '0.40 - 0.50', marginal: '0.50 - 0.55 or 0.35 - 0.40',
    reject: '> 0.55 or < 0.35', digits: 3 },
  { key: 'estimated_slump_mm', label: 'Slump', unit: ' mm',
    good: '50 - 125', marginal: '125 - 150 or < 50',
    reject: '> 150', digits: 1 },
  { key: 'temperature_c', label: 'Mix temperature', unit: ' °C',
    good: '< 35', marginal: '35 - 40', reject: '> 40', digits: 2,
    from: 'sensor_raw' },
];

function fmt(v, digits, unit) {
  if (v === undefined || v === null || Number.isNaN(v)) return 'not measured';
  return Number(v).toFixed(digits) + (unit || '');
}

function hr(doc, y) {
  doc.save().strokeColor(COLORS.rule).lineWidth(0.5)
     .moveTo(50, y).lineTo(545, y).stroke().restore();
}

/**
 * @param {object} opts
 * @param {Array}  opts.records  measurement records (newest last)
 * @param {object} opts.meta     { student, section, roll, site }
 * @param {stream.Writable} out  destination stream
 */
function generateReport({ records, meta }, out) {
  // bufferPages is REQUIRED for the page-footer pass at the end: without it,
  // bufferedPageRange() reports only the current page, the "Page N of M"
  // footer is wrong, and switchToPage() appends blank pages instead of
  // revisiting existing ones.
  const doc = new PDFKit({ size: 'A4', margin: 50, bufferPages: true,
                           info: { Title: 'ConcreSense Site Audit Report',
                                   Author: meta.student || 'ConcreSense' } });
  doc.pipe(out);

  const latest = records.length ? records[records.length - 1] : null;

  // ------------------------------------------------------------- header
  doc.fontSize(20).fillColor(COLORS.ink).text('ConcreSense', 50, 50);
  doc.fontSize(10).fillColor(COLORS.muted)
     .text('On-site fresh-concrete screening report', 50, 74);
  // width widened to 185 (was 145): at 9pt the "Generated ... UTC" string
  // exceeds 145pt and wraps to a second line, which then overlaps the
  // "Reference standard" line 14pt below it.
  doc.fontSize(9).fillColor(COLORS.muted)
     .text(`Generated ${new Date().toISOString().replace('T', ' ').replace(/\..*/, '')} UTC`,
           360, 52, { width: 185, align: 'right', lineBreak: false })
     .text(`Reference standard: IS 456:2000`, 360, 66,
           { width: 185, align: 'right', lineBreak: false });

  hr(doc, 96);

  // Provenance banner. Deliberately near the top and not in fine print: this
  // document could otherwise be mistaken for a lab certificate.
  doc.rect(50, 106, 495, 46).fillColor('#fff8e1').fill();
  doc.fillColor('#8a6d00').fontSize(8.5)
     .text('PROVENANCE — READ BEFORE USE', 60, 113)
     .fillColor(COLORS.ink)
     .text('This device screens fresh cement paste/mortar using sensor fusion and an on-device '
         + 'classifier trained on a PHYSICS-SIMULATED dataset. It has NOT been validated against '
         + 'laboratory-tested concrete cylinders, and does not replace testing to IS 516 / IS 1199.',
           60, 125, { width: 475 });

  let y = 168;

  // ------------------------------------------------------------- meta
  doc.fontSize(11).fillColor(COLORS.ink).text('Test details', 50, y);
  y += 18;
  // Count simulated rows so the report states its own composition rather than
  // presenting simulated and measured tests as one undifferentiated set.
  const nSim = records.filter(r => r.data_source === 'simulated_onboard').length;
  const metaRows = [
    ['Prepared by', `${meta.student || '-'}   (${meta.section || '-'}, ${meta.roll || '-'})`],
    ['Device ID', latest?.device_id || '-'],
    ['Firmware', latest?.firmware || '-'],
    ['Tests in this report', nSim
      ? `${records.length}  (${nSim} SIMULATED, ${records.length - nSim} measured)`
      : String(records.length)],
    ['Report period', records.length
      ? `${records[0].received_at || '-'}  to  ${latest.received_at || '-'}`
      : '-'],
  ];
  doc.fontSize(9);
  for (const [k, v] of metaRows) {
    doc.fillColor(COLORS.muted).text(k, 50, y, { width: 140 });
    doc.fillColor(COLORS.ink).text(v, 195, y, { width: 350 });
    y += 14;
  }

  // Verdict distribution across the whole report (not just the latest test),
  // computed the same way as the dashboard's /api/summary so the two never
  // disagree. A tiny inline bar chart -- real counts, not decoration.
  {
    const counts = { GOOD: 0, MARGINAL: 0, REJECT: 0, UNKNOWN: 0 };
    let disagreements = 0;
    for (const r of records) {
      const c = r.classification || {};
      const v = c.rule_result || 'UNKNOWN';
      if (counts[v] !== undefined) counts[v]++;
      if (c.agreement === false) disagreements++;
    }
    const agreeN = records.length - disagreements;
    const agreePct = records.length ? Math.round((100 * agreeN) / records.length) : 0;

    doc.fillColor(COLORS.muted).fontSize(9).text('Verdict distribution', 50, y + 10, { width: 140 });
    const maxC = Math.max(1, counts.GOOD, counts.MARGINAL, counts.REJECT);
    const baseY = y + 40, barW = 14, slot = 52, x0 = 195, maxH = 26;
    ['GOOD', 'MARGINAL', 'REJECT'].forEach((k, i) => {
      const h = counts[k] > 0 ? Math.max(3, (counts[k] / maxC) * maxH) : 0;
      const x = x0 + i * slot;
      if (h > 0) doc.roundedRect(x, baseY - h, barW, h, 2).fillColor(COLORS[k]).fill();
      doc.fontSize(7.5).fillColor(COLORS.ink)
         .text(String(counts[k]), x - 10, baseY - Math.max(h, 3) - 10, { width: barW + 20, align: 'center' });
      // Label box wider than the slot spacing on purpose (centered text,
      // not a border) -- "MARGINAL" at 6.5pt needs ~30pt and must not wrap.
      doc.fontSize(6.5).fillColor(COLORS.muted)
         .text(k, x - 18, baseY + 3, { width: barW + 36, align: 'center', lineBreak: false });
    });
    // "<->" rather than "↔": PDFKit's base Helvetica (WinAnsi encoding) has
    // no glyph for U+2194 and silently prints garbage ("!”") instead.
    doc.fontSize(8.5).fillColor(COLORS.muted)
       .text(`Model <-> rules agreement: ${agreeN}/${records.length} (${agreePct}%)`, 355, y + 12, { width: 190 });
    y += 54;
  }

  hr(doc, y);
  y += 14;

  // ------------------------------------------------------- latest verdict
  doc.fontSize(11).fillColor(COLORS.ink).text('Most recent test', 50, y);
  y += 20;

  if (!latest) {
    doc.fontSize(10).fillColor(COLORS.muted)
       .text('No measurements have been recorded. Nothing to report.', 50, y);
    doc.end();
    return;
  }

  const cls = latest.classification || {};
  const verdict = cls.rule_result || 'UNKNOWN';

  // A simulated test must be unmistakable on the page itself, not only in the
  // metadata table someone might skim past.
  if (latest.data_source === 'simulated_onboard') {
    doc.rect(50, y, 495, 18).fillColor('#fdecea').fill();
    doc.fillColor(COLORS.REJECT).fontSize(9)
       .text('THIS TEST WAS SIMULATED ON-DEVICE — no sensors were involved.',
             56, y + 5, { width: 483 });
    y += 26;
  }
  doc.rect(50, y, 160, 44).fillColor(COLORS[verdict] || COLORS.UNKNOWN).fill();
  doc.fillColor('#ffffff').fontSize(20).text(verdict, 50, y + 13,
                                             { width: 160, align: 'center' });

  doc.fontSize(8.5).fillColor(COLORS.muted)
     .text('Verdict is determined by the IS 456:2000 rule engine.', 225, y + 6,
           { width: 320 })
     .text(`On-device model predicted: ${cls.model_result || '-'}`
         + (cls.confidence != null
             ? ` (confidence ${(cls.confidence * 100).toFixed(1)}%)` : ''),
           225, y + 20, { width: 320 });
  if (cls.agreement === false) {
    doc.fillColor(COLORS.REJECT)
       .text('Model and rules DISAGREED on this test — rules take precedence.',
             225, y + 34, { width: 320 });
  }
  y += 60;

  // ------------------------------------------------------- limits table
  doc.fontSize(11).fillColor(COLORS.ink).text('Measured vs specified limits', 50, y);
  y += 18;

  const cols = [50, 175, 255, 355, 545];
  doc.fontSize(8.5).fillColor(COLORS.muted);
  doc.text('Parameter', cols[0], y);
  doc.text('Measured', cols[1], y);
  doc.text('Acceptable (GOOD)', cols[2], y);
  doc.text('Outside specification', cols[3], y);
  y += 12;
  hr(doc, y); y += 6;

  doc.fontSize(9);
  for (const lim of LIMITS) {
    const src = lim.from === 'sensor_raw' ? latest.sensor_raw : latest.features;
    const val = src ? src[lim.key] : undefined;
    doc.fillColor(COLORS.ink).text(lim.label, cols[0], y, { width: 120 });
    doc.fillColor(val == null ? COLORS.muted : COLORS.ink)
       .text(fmt(val, lim.digits, lim.unit), cols[1], y, { width: 75 });
    doc.fillColor(COLORS.muted).text(lim.good, cols[2], y, { width: 95 });
    doc.text(lim.reject, cols[3], y, { width: 185 });
    y += 15;
  }

  y += 4; hr(doc, y); y += 14;

  // ------------------------------------------------------- raw + features
  doc.fontSize(11).fillColor(COLORS.ink).text('Supporting measurements', 50, y);
  y += 18;
  const raw = latest.sensor_raw || {};
  const feat = latest.features || {};
  const support = [
    ['Moisture sensor', fmt(raw.moisture_mv, 1, ' mV')],
    ['Load cell', fmt(raw.load_counts, 0, ' counts')],
    ['Penetration force', fmt(feat.penetration_force_n, 3, ' N')],
    ['Shear yield stress', fmt(feat.yield_stress_pa, 0, ' Pa')],
    ['Vibration RMS', fmt(raw.vibration_rms_g, 4, ' g')],
    ['Dominant frequency', fmt(feat.vib_dominant_hz, 2, ' Hz')],
    ['Spectral entropy', fmt(feat.vib_spectral_entropy, 3)],
    ['Damping ratio', fmt(feat.vib_damping_ratio, 4)],
  ];
  doc.fontSize(9);
  let col = 0;
  const startY = y;
  for (const [k, v] of support) {
    const x = col < 4 ? 50 : 300;
    const yy = startY + (col % 4) * 14;
    doc.fillColor(COLORS.muted).text(k, x, yy, { width: 130 });
    doc.fillColor(COLORS.ink).text(v, x + 135, yy, { width: 110 });
    col++;
  }
  y = startY + 4 * 14 + 8;

  // ------------------------------------------------------------- location
  hr(doc, y); y += 14;
  doc.fontSize(11).fillColor(COLORS.ink).text('Location', 50, y);
  y += 18;
  const loc = latest.location || {};
  doc.fontSize(9);
  if (loc.fix) {
    doc.fillColor(COLORS.ink)
       .text(`${Number(loc.latitude).toFixed(6)}, ${Number(loc.longitude).toFixed(6)}`
           + `   (HDOP ${loc.hdop ?? '-'}, ${loc.satellites ?? '-'} satellites)`, 50, y);
  } else {
    // Never print 0,0 as if it were a location.
    doc.fillColor(COLORS.muted)
       .text('No GPS fix at time of test — coordinates not recorded. '
           + '(NEO-6M cannot acquire a fix indoors.)', 50, y, { width: 495 });
  }
  y += 24;

  // ------------------------------------------------------------- history
  if (records.length > 1) {
    hr(doc, y); y += 14;
    doc.fontSize(11).fillColor(COLORS.ink).text('Test history', 50, y);
    y += 18;

    const hcols = [50, 90, 210, 265, 320, 380, 450];
    doc.fontSize(8.5).fillColor(COLORS.muted);
    ['#', 'Timestamp (UTC)', 'w/c', 'Slump', 'Temp', 'Rules', 'Model']
      .forEach((h, i) => doc.text(h, hcols[i], y));
    y += 12; hr(doc, y); y += 5;

    doc.fontSize(8.5);
    for (const r of records.slice(-22).reverse()) {
      if (y > 760) { doc.addPage(); y = 50; }
      const rc = r.classification || {}, rf = r.features || {}, rr = r.sensor_raw || {};
      doc.fillColor(COLORS.ink).text(String(r.seq ?? '-'), hcols[0], y);
      doc.fillColor(COLORS.muted)
         .text((r.timestamp_utc || '').replace('T', ' ').replace('Z', ''), hcols[1], y,
               { width: 115 });
      doc.fillColor(COLORS.ink)
         .text(fmt(rf.estimated_wc_ratio, 3), hcols[2], y, { width: 50 })
         .text(fmt(rf.estimated_slump_mm, 0), hcols[3], y, { width: 50 })
         .text(fmt(rr.temperature_c, 1), hcols[4], y, { width: 55 });
      doc.fillColor(COLORS[rc.rule_result] || COLORS.UNKNOWN)
         .text(rc.rule_result || '-', hcols[5], y, { width: 65 });
      doc.fillColor(COLORS[rc.model_result] || COLORS.UNKNOWN)
         .text(rc.model_result || '-', hcols[6], y, { width: 70 });
      y += 12;
    }
  }

  // ------------------------------------------------------------- footer
  // bufferedPageRange() must be read BEFORE flushing; switchToPage() can only
  // revisit pages that are still buffered. doc.end() flushes them afterwards.
  const range = doc.bufferedPageRange();
  for (let i = range.start; i < range.start + range.count; i++) {
    doc.switchToPage(i);

    // Zero the bottom margin for the footer only. A4 is 841.89pt tall; with a
    // 50pt margin the text area ends at ~792, so writing a footer below that
    // makes PDFKit auto-insert a NEW page for the overflow -- which silently
    // turned a 1-page report into 3 blank-tailed pages and left the "Page N of
    // M" count wrong. Restored immediately after.
    const savedBottom = doc.page.margins.bottom;
    doc.page.margins.bottom = 0;

    doc.fontSize(7.5).fillColor(COLORS.muted)
       .text('ConcreSense screening report — physics-simulated model, '
           + 'not validated against laboratory-tested concrete.',
             50, doc.page.height - 38, { width: 400, lineBreak: false })
       .text(`Page ${i + 1} of ${range.count}`, 450, doc.page.height - 38,
             { width: 95, align: 'right', lineBreak: false });

    doc.page.margins.bottom = savedBottom;
  }

  doc.end();
}

module.exports = { generateReport, LIMITS };
