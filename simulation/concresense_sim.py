#!/usr/bin/env python3
"""
ConcreSense — animated end-to-end pipeline simulation.

Shows the full workflow the way the firmware actually executes it:

    SENSE  ->  FEATURES  ->  PHYSICS  ->  CLASSIFY  ->  PUBLISH

Every number on screen is produced by the SAME physics module the training
dataset uses (tinyml_model/concresense_physics.py), which in turn mirrors the
C++ in firmware/concresense/src/physics/calibration.cpp. Nothing here is
hard-coded for the demo: change a slider and the physics re-derives everything.

WHAT THIS IS
    A simulation of the measurement chain, driven by published physical
    relations, useful for showing and testing the pipeline without hardware.

WHAT THIS IS NOT
    Measured data. No real concrete has been tested. The relations are
    published; the sensor forward models are plausible, not fitted. The banner
    on the figure says so, on purpose.

Usage
    python3 simulation/concresense_sim.py                    # scripted demo
    python3 simulation/concresense_sim.py --mode interactive # sliders
    python3 simulation/concresense_sim.py --export demo.mp4  # record a video
    python3 simulation/concresense_sim.py --export demo.gif --scenarios 4
"""

from __future__ import annotations

import argparse
import json

import pathlib
import sys

import numpy as np

# The shared physics lives with the dataset generator so the two can never drift.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tinyml_model"))

import concresense_physics as phys  # noqa: E402

import matplotlib  # noqa: E402

# ----------------------------------------------------------------- palette
BG = "#11151c"
PANEL = "#1a212c"
GRID = "#2a3444"
FG = "#e6edf3"
MUTED = "#8b98a9"
ACCENT = "#4aa3ff"

CLASS_COLOR = {
    "GOOD": "#3fb950",
    "MARGINAL": "#d29922",
    "REJECT": "#f85149",
    "UNKNOWN": MUTED,
}

STAGES = ["SENSE", "FEATURES", "PHYSICS", "CLASSIFY", "PUBLISH"]

# Frames spent on each stage, then on holding the verdict. Tuned so a viewer can
# actually read each stage rather than the pipeline flashing past.
FRAMES_PER_STAGE = 7
HOLD_FRAMES = 16
CYCLE_FRAMES = len(STAGES) * FRAMES_PER_STAGE + HOLD_FRAMES


# --------------------------------------------------------------- scenarios
# Deliberately includes both threshold gaps the source documents left open
# (w/c 0.35-0.40 and slump < 50mm). Those cases would fall through to GOOD under
# the docs as written, so demonstrating them is the clearest way to show the
# rule engine is doing something the spec did not.
SCENARIOS = [
    ("Well-proportioned M25 mix",      0.45,  90.0, 30.0),
    ("Slightly wet, workable",         0.52, 120.0, 32.0),
    ("Hot-weather pour",               0.47, 100.0, 37.5),
    ("Too dry (doc gap 0.35-0.40)",    0.37,  85.0, 29.0),
    ("Stiff mix (doc gap slump<50)",   0.44,  32.0, 28.0),
    ("Excess water - segregating",     0.62, 185.0, 31.0),
    ("Overheated batch",               0.48, 110.0, 43.0),
]


class Pipeline:
    """
    One measurement cycle: true mix state -> simulated sensors -> what the
    firmware would compute from them.

    The split matters and is the whole point of the demo. `truth` is what the
    concrete actually is; `measured` is what the device can see. The device only
    ever gets `measured`, so the error between the two is the honest measure of
    what this instrument can and cannot do.
    """

    def __init__(self, seed: int = 7):
        self.rng = np.random.default_rng(seed)

    def run(self, wc_true: float, slump_true: float, temp_true: float) -> dict:
        rng = self.rng

        # --- 1. SENSE: forward-model each sensor from the true state
        v_water = phys.wc_to_water_volume_fraction(np.array([wc_true]))
        eps_true = phys.lichtenecker_eps_mix(v_water, np.array([temp_true]))
        moisture_mv = float(phys.eps_mix_to_millivolts(
            eps_true, np.array([temp_true]), rng)[0])

        tau0_true = float(phys.slump_to_yield_stress(np.array([slump_true]))[0])
        force_n = float(phys.yield_stress_to_force_n(np.array([tau0_true]), rng)[0])

        # DS18B20 at 12-bit: 0.0625 degC steps.
        temp_meas = float(np.round((temp_true + rng.normal(0, 0.22)) / 0.0625) * 0.0625)

        rms, dom, ent, _damp = phys.vibration_signature(
            np.array([wc_true]), np.array([slump_true]), rng, 1)
        waveform = phys.synth_vibration_waveform(
            float(dom[0]), float(rms[0]), float(ent[0]), rng)

        # --- 2. FEATURES: the same DSP the firmware runs
        feats, freqs, psd = phys.extract_features(waveform)

        # --- 3. PHYSICS: invert the sensor readings (device-side only)
        eps_meas = float(phys.millivolts_to_eps_mix(moisture_mv, temp_meas))
        vw_meas = float(phys.lichtenecker_water_fraction(eps_meas, temp_meas))
        wc_meas = float(phys.water_fraction_to_wc(vw_meas))

        tau0_meas = float(phys.force_n_to_yield_stress(force_n))
        slump_meas = float(phys.yield_stress_to_slump(tau0_meas))

        # --- 4. CLASSIFY: IS 456 rule engine on the MEASURED values
        verdict = phys.classify_scalar(wc_meas, slump_meas, temp_meas)
        truth_verdict = phys.classify_scalar(wc_true, slump_true, temp_true)
        reasons = phys.reason_for_class(wc_meas, slump_meas, temp_meas)

        # --- 5. PUBLISH
        payload = {
            "device_id": "CONCRESENSE_ESP32_001",
            "location": {"latitude": 12.971598, "longitude": 77.594566,
                         "hdop": 1.2, "source": "SIMULATED"},
            "sensor_raw": {
                "moisture_mv": round(moisture_mv, 1),
                "temperature_c": round(temp_meas, 3),
                "penetration_force_n": round(force_n, 4),
                "vibration_rms_g": round(feats["rms"], 4),
            },
            "features": {
                "estimated_wc_ratio": round(wc_meas, 3),
                "estimated_slump_mm": round(slump_meas, 1),
                "yield_stress_pa": round(tau0_meas, 1),
                "vib_dominant_hz": round(feats["dominant_hz"], 2),
                "vib_spectral_entropy": round(feats["entropy"], 3),
            },
            "classification": {"result": verdict},
            "standard_compliance": "IS 456:2000",
            "data_source": "simulated_physics",
        }

        return dict(
            truth=dict(wc=wc_true, slump=slump_true, temp=temp_true,
                       verdict=truth_verdict, tau0=tau0_true),
            measured=dict(wc=wc_meas, slump=slump_meas, temp=temp_meas,
                          verdict=verdict, tau0=tau0_meas,
                          moisture_mv=moisture_mv, force_n=force_n),
            feats=feats, freqs=freqs, psd=psd, waveform=waveform,
            reasons=reasons, payload=payload,
        )


# ------------------------------------------------------------------ figure
def build_figure():
    import matplotlib.pyplot as plt
    from matplotlib.gridspec import GridSpec

    fig = plt.figure(figsize=(16, 9), facecolor=BG)
    fig.canvas.manager.set_window_title("ConcreSense — end-to-end simulation")

    gs = GridSpec(
        4, 3, figure=fig,
        height_ratios=[0.9, 1.5, 1.5, 0.9],
        width_ratios=[1.25, 1.0, 1.0],
        hspace=0.55, wspace=0.28,
        left=0.045, right=0.972, top=0.90, bottom=0.06,
    )

    ax_pipe = fig.add_subplot(gs[0, :])
    ax_wave = fig.add_subplot(gs[1, 0])
    ax_fft = fig.add_subplot(gs[1, 1])
    ax_verdict = fig.add_subplot(gs[1:3, 2])
    ax_bars = fig.add_subplot(gs[2, 0])
    ax_feats = fig.add_subplot(gs[2, 1])
    ax_json = fig.add_subplot(gs[3, :])

    for ax in (ax_pipe, ax_verdict, ax_json):
        ax.set_axis_off()
    for ax in (ax_wave, ax_fft, ax_bars, ax_feats):
        ax.set_facecolor(PANEL)
        for s in ax.spines.values():
            s.set_color(GRID)
        ax.tick_params(colors=MUTED, labelsize=8)
        ax.grid(True, color=GRID, lw=0.6, alpha=0.6)
        ax.set_axisbelow(True)

    fig.text(0.045, 0.962, "ConcreSense", color=FG, fontsize=22, fontweight="bold")
    fig.text(0.185, 0.968, "on-site fresh-concrete screening  ·  ESP32 + sensor fusion + IS 456:2000",
             color=MUTED, fontsize=10.5)
    fig.text(0.045, 0.934,
             "SIMULATION — physics-derived, not measured. No real concrete has been tested.",
             color="#d29922", fontsize=9.5, style="italic")

    return fig, dict(pipe=ax_pipe, wave=ax_wave, fft=ax_fft, verdict=ax_verdict,
                     bars=ax_bars, feats=ax_feats, json=ax_json)


def draw_pipeline(ax, active_idx: int):
    """Five-stage flow strip; the active stage is filled and outlined."""
    from matplotlib.patches import FancyBboxPatch

    ax.clear()
    ax.set_axis_off()
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 1)

    w, gap = 1.62, 0.47
    x = 0.15
    for i, name in enumerate(STAGES):
        done = i < active_idx
        live = i == active_idx
        face = ACCENT if live else (PANEL if not done else "#1e2f45")
        edge = ACCENT if live else (GRID if not done else "#2f6ea8")
        txt = "#08121e" if live else (FG if done else MUTED)

        ax.add_patch(FancyBboxPatch(
            (x, 0.28), w, 0.46, boxstyle="round,pad=0.02,rounding_size=0.09",
            facecolor=face, edgecolor=edge, linewidth=2.0 if live else 1.2))
        ax.text(x + w / 2, 0.51, name, ha="center", va="center",
                color=txt, fontsize=11.5,
                fontweight="bold" if live else "normal")

        if i < len(STAGES) - 1:
            ax.annotate("", xy=(x + w + gap - 0.07, 0.51), xytext=(x + w + 0.07, 0.51),
                        arrowprops=dict(arrowstyle="-|>", color=ACCENT if done else GRID,
                                        lw=1.9, shrinkA=0, shrinkB=0))
        x += w + gap


def draw_verdict(ax, res, stage_idx: int):
    from matplotlib.patches import FancyBboxPatch

    ax.clear()
    ax.set_axis_off()
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)

    revealed = stage_idx >= STAGES.index("CLASSIFY")
    verdict = res["measured"]["verdict"] if revealed else "…"
    col = CLASS_COLOR[res["measured"]["verdict"]] if revealed else GRID

    ax.add_patch(FancyBboxPatch(
        (0.02, 0.62), 0.96, 0.33, boxstyle="round,pad=0.02,rounding_size=0.05",
        facecolor=col if revealed else PANEL, edgecolor=col, linewidth=2.2))
    ax.text(0.5, 0.785, verdict, ha="center", va="center",
            color="#08121e" if revealed else MUTED,
            fontsize=30, fontweight="bold")
    ax.text(0.5, 0.985, "IS 456:2000 VERDICT", ha="center", va="top",
            color=MUTED, fontsize=9.5, fontweight="bold")

    if not revealed:
        return

    ax.text(0.03, 0.55, "why:", color=MUTED, fontsize=9.5, fontweight="bold")
    y = 0.485
    for r in res["reasons"][:4]:
        ax.text(0.06, y, f"• {r}", color=FG, fontsize=9.0, va="top")
        y -= 0.058

    # Truth-vs-measured. This is the honest part of the display: the device is
    # judged on what it could actually see, and any disagreement with the true
    # mix state is shown rather than hidden.
    t, m = res["truth"], res["measured"]
    ax.text(0.03, y - 0.03, "true vs measured", color=MUTED, fontsize=9.5,
            fontweight="bold")
    y -= 0.10
    rows = [
        ("w/c", t["wc"], m["wc"], "{:.3f}"),
        ("slump", t["slump"], m["slump"], "{:.0f} mm"),
        ("temp", t["temp"], m["temp"], "{:.1f} C"),
    ]
    for label, tv, mv, fmt in rows:
        ax.text(0.06, y, label, color=MUTED, fontsize=9)
        ax.text(0.42, y, fmt.format(tv), color=MUTED, fontsize=9, ha="right")
        ax.text(0.52, y, "→", color=GRID, fontsize=9, ha="center")
        ax.text(0.95, y, fmt.format(mv), color=FG, fontsize=9, ha="right")
        y -= 0.055

    agree = t["verdict"] == m["verdict"]
    ax.text(0.06, y - 0.02,
            "device agrees with true class"
            if agree else f"MISREAD — true class is {t['verdict']}",
            color=CLASS_COLOR["GOOD"] if agree else CLASS_COLOR["REJECT"],
            fontsize=9.0, fontweight="bold")


def draw_bars(ax, res, stage_idx: int):
    ax.clear()
    ax.set_facecolor(PANEL)
    for s in ax.spines.values():
        s.set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=8)
    ax.set_title("derived properties  (measured vs IS 456 window)",
                 color=FG, fontsize=10, pad=8, loc="left")

    if stage_idx < STAGES.index("PHYSICS"):
        ax.text(0.5, 0.5, "awaiting physics…", transform=ax.transAxes,
                ha="center", va="center", color=MUTED, fontsize=11)
        ax.set_xticks([])
        ax.set_yticks([])
        return

    m = res["measured"]
    # Each bar is normalised to its own GOOD window so one axis can show three
    # quantities with different units; the shaded band is the acceptable range.
    specs = [
        ("w/c",   m["wc"],    0.30, 0.70, 0.40, 0.50),
        ("slump", m["slump"], 0.0,  260.0, 50.0, 125.0),
        ("temp",  m["temp"],  10.0, 50.0, 10.0, 35.0),
    ]
    ypos = np.arange(len(specs))
    for i, (name, val, lo, hi, glo, ghi) in enumerate(specs):
        frac = (val - lo) / (hi - lo)
        gl, gh = (glo - lo) / (hi - lo), (ghi - lo) / (hi - lo)
        ax.barh(i, gh - gl, left=gl, height=0.55,
                color=CLASS_COLOR["GOOD"], alpha=0.20)
        inside = gl <= frac <= gh
        ax.barh(i, 0.012, left=frac - 0.006, height=0.72,
                color=CLASS_COLOR["GOOD"] if inside else CLASS_COLOR["REJECT"])
        unit = {"w/c": "{:.3f}", "slump": "{:.0f} mm", "temp": "{:.1f} C"}[name]
        ax.text(1.015, i, unit.format(val), color=FG, fontsize=9,
                va="center", transform=ax.get_yaxis_transform())

    ax.set_yticks(ypos)
    ax.set_yticklabels([s[0] for s in specs], color=FG, fontsize=9)
    ax.set_xlim(0, 1)
    ax.set_ylim(-0.6, len(specs) - 0.4)
    ax.set_xticks([])
    ax.invert_yaxis()
    ax.grid(False)


def draw_feats(ax, res, stage_idx: int):
    ax.clear()
    ax.set_facecolor(PANEL)
    for s in ax.spines.values():
        s.set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=8)
    ax.set_title("vibration features → model input", color=FG, fontsize=10,
                 pad=8, loc="left")
    ax.set_axis_off()

    if stage_idx < STAGES.index("FEATURES"):
        ax.text(0.5, 0.5, "awaiting FFT…", transform=ax.transAxes,
                ha="center", va="center", color=MUTED, fontsize=11)
        return

    f = res["feats"]
    rows = [
        ("RMS (AC)", f"{f['rms']:.4f} g"),
        ("peak-to-peak", f"{f['p2p']:.4f} g"),
        ("dominant freq", f"{f['dominant_hz']:.2f} Hz"),
        ("spectral centroid", f"{f['centroid_hz']:.2f} Hz"),
        ("spectral entropy", f"{f['entropy']:.3f}"),
        ("damping ratio", f"{f['damping']:.4f}"),
    ]
    y = 0.92
    for k, v in rows:
        ax.text(0.03, y, k, color=MUTED, fontsize=9.5, va="top",
                transform=ax.transAxes)
        ax.text(0.97, y, v, color=FG, fontsize=9.5, va="top", ha="right",
                transform=ax.transAxes, family="monospace")
        y -= 0.145

    if f["dominant_hz"] > 40.0:
        ax.text(0.03, y, "⚠ near 44 Hz DLPF corner — filter-shaped",
                color="#d29922", fontsize=8.5, va="top", transform=ax.transAxes)


def draw_json(ax, res, stage_idx: int):
    ax.clear()
    ax.set_axis_off()
    published = stage_idx >= STAGES.index("PUBLISH")
    ax.text(0.0, 1.02, "MQTT  concresense/site/CONCRESENSE_ESP32_001/test",
            color=MUTED, fontsize=9.5, fontweight="bold",
            transform=ax.transAxes, va="top")
    if not published:
        ax.text(0.0, 0.72, "waiting for classification…", color=GRID,
                fontsize=9.5, transform=ax.transAxes, va="top",
                family="monospace")
        return

    p = res["payload"]
    compact = {
        "sensor_raw": p["sensor_raw"],
        "features": p["features"],
        "classification": p["classification"],
    }
    txt = json.dumps(compact, separators=(", ", ": "))
    # Wrap so a long payload never runs off the figure edge.
    line, lines = "", []
    for tok in txt.split(", "):
        if len(line) + len(tok) > 150:
            lines.append(line)
            line = tok
        else:
            line = f"{line}, {tok}" if line else tok
    lines.append(line)

    ax.text(0.0, 0.76, "\n".join(lines[:3]), color=CLASS_COLOR["GOOD"],
            fontsize=8.2, transform=ax.transAxes, va="top", family="monospace")


# ------------------------------------------------------------------ driver
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["auto", "interactive"], default="auto")
    ap.add_argument("--export", default=None,
                    help="write an .mp4 or .gif instead of showing a window")
    ap.add_argument("--scenarios", type=int, default=len(SCENARIOS),
                    help="how many scenarios to run in auto mode")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    if args.export:
        matplotlib.use("Agg")  # must be set before pyplot is imported for real

    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation

    pipeline = Pipeline(args.seed)
    fig, axes = build_figure()

    scenarios = SCENARIOS[: max(1, min(args.scenarios, len(SCENARIOS)))]
    state = {"res": None, "name": "", "idx": -1}

    def render(res, name, stage_idx):
        draw_pipeline(axes["pipe"], stage_idx)

        ax = axes["wave"]
        ax.clear()
        ax.set_facecolor(PANEL)
        for s in ax.spines.values():
            s.set_color(GRID)
        ax.tick_params(colors=MUTED, labelsize=8)
        ax.grid(True, color=GRID, lw=0.6, alpha=0.6)
        ax.set_title(f"MPU-6050 burst — {name}", color=FG, fontsize=10,
                     pad=8, loc="left")
        t = np.arange(len(res["waveform"])) / phys.IMU_SAMPLE_RATE_HZ
        ax.plot(t, res["waveform"], color=ACCENT, lw=1.0)
        ax.axhline(float(np.mean(res["waveform"])), color="#d29922", lw=0.9,
                   ls="--", alpha=0.9)
        ax.set_xlabel("s", color=MUTED, fontsize=8)
        ax.set_ylabel("|a|  (g)", color=MUTED, fontsize=8)
        ax.text(0.985, 0.06,
                "dashed = DC (gravity), removed before FFT",
                transform=ax.transAxes, ha="right", color=MUTED, fontsize=7.8)

        ax = axes["fft"]
        ax.clear()
        ax.set_facecolor(PANEL)
        for s in ax.spines.values():
            s.set_color(GRID)
        ax.tick_params(colors=MUTED, labelsize=8)
        ax.grid(True, color=GRID, lw=0.6, alpha=0.6)
        ax.set_title("spectrum (256-pt FFT @ 200 Hz)", color=FG, fontsize=10,
                     pad=8, loc="left")
        if stage_idx >= STAGES.index("FEATURES"):
            ax.semilogy(res["freqs"], np.maximum(res["psd"], 1e-9),
                        color=CLASS_COLOR["GOOD"], lw=0.9)
            ax.axvline(res["feats"]["dominant_hz"], color="#d29922", lw=1.2, ls="--")
            ax.axvline(phys.DLPF_CORNER_HZ, color=CLASS_COLOR["REJECT"], lw=1.0,
                       ls=":", alpha=0.85)
            ax.text(phys.DLPF_CORNER_HZ - 1.5, ax.get_ylim()[1] * 0.25, "DLPF 44Hz",
                    color=CLASS_COLOR["REJECT"], fontsize=7.5, rotation=90, ha="right")
        else:
            ax.text(0.5, 0.5, "awaiting FFT…", transform=ax.transAxes,
                    ha="center", va="center", color=MUTED, fontsize=11)
        ax.set_xlabel("Hz", color=MUTED, fontsize=8)
        ax.set_xlim(0, phys.IMU_SAMPLE_RATE_HZ / 2)

        draw_feats(axes["feats"], res, stage_idx)
        draw_bars(axes["bars"], res, stage_idx)
        draw_verdict(axes["verdict"], res, stage_idx)
        draw_json(axes["json"], res, stage_idx)

    if args.mode == "interactive":
        from matplotlib.widgets import Slider

        fig.subplots_adjust(bottom=0.20)
        sax = [fig.add_axes((0.08, 0.135 - i * 0.038, 0.34, 0.022),
                            facecolor=PANEL) for i in range(3)]
        s_wc = Slider(sax[0], "w/c", 0.28, 0.70, valinit=0.45, color=ACCENT)
        s_sl = Slider(sax[1], "slump mm", 0.0, 240.0, valinit=90.0, color=ACCENT)
        s_tp = Slider(sax[2], "temp C", 12.0, 48.0, valinit=30.0, color=ACCENT)
        for s in (s_wc, s_sl, s_tp):
            s.label.set_color(FG)
            s.valtext.set_color(FG)

        def on_change(_):
            res = pipeline.run(s_wc.val, s_sl.val, s_tp.val)
            render(res, "interactive", len(STAGES) - 1)
            fig.canvas.draw_idle()

        for s in (s_wc, s_sl, s_tp):
            s.on_changed(on_change)
        on_change(None)
        plt.show()
        return 0

    total = len(scenarios) * CYCLE_FRAMES

    def update(frame):
        idx = frame // CYCLE_FRAMES
        within = frame % CYCLE_FRAMES
        if idx != state["idx"]:
            name, wc, sl, tp = scenarios[idx]
            state.update(idx=idx, name=name, res=pipeline.run(wc, sl, tp))
        stage_idx = min(within // FRAMES_PER_STAGE, len(STAGES) - 1)
        render(state["res"], state["name"], stage_idx)
        return []

    anim = FuncAnimation(fig, update, frames=total, interval=1000 // args.fps,
                         blit=False, repeat=not args.export)

    if args.export:
        out = pathlib.Path(args.export)
        if out.suffix == ".gif":
            from matplotlib.animation import PillowWriter
            writer = PillowWriter(fps=args.fps)
        else:
            from matplotlib.animation import FFMpegWriter
            writer = FFMpegWriter(fps=args.fps, bitrate=2600)
        print(f"rendering {total} frames -> {out} …")
        anim.save(str(out), writer=writer, dpi=110,
                  savefig_kwargs={"facecolor": BG})
        print(f"wrote {out}  ({out.stat().st_size / 1e6:.1f} MB)")
    else:
        plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
