# ConcreSense — Presentation Night-Before Briefing

*Read time: ~5 min. Skim the bold lines if you're really pressed.*

---

## 1. How to frame "some sensors work, some don't, model is synthetic"

Research on capstone/hackathon judging is consistent on one point: **judges score your understanding of limitations, not the absence of limitations.** Explicitly naming what's not done, and why, reads as engineering maturity — not weakness. Evaluators specifically look for how you frame current constraints and your plan forward, and transparency about what works vs. what's still in progress is valued over a polished-but-vague narrative. Completion is only one axis of the rubric; problem framing, methodology, and critical self-assessment are separately scored.

**Concrete lines you can actually say tomorrow:**

- *"Four of six sensing channels — load cell, temperature, OLED, GPS — are live on hardware tonight. Moisture and vibration calibration are in progress; I'm demoing the full inference-to-dashboard pipeline using the firmware's built-in `sim` command, which pushes real simulated measurements through the exact same MQTT and TinyML code path the live sensors use."*
- *"The classifier is trained on a physics-simulated dataset — 900 rows generated from the IS 456:2000 rules and Lichtenecker/Bingham models, not real concrete pours. I report that as synthetic-set separability, not classification accuracy, because a model trained on rule-generated labels will trivially reproduce those rules — that number would measure nothing about real concrete."*
- *"GPS gets no satellite fix indoors — that's expected physics, not a bug, and I can explain exactly why if you'd like."* (see §3 below for the crisp version)
- *"The near-term validation gap is real-concrete cylinders — that's explicitly future work, not something I'm claiming today."*
- *"I've deliberately built this to fail loudly rather than fake gracefully — the `sim` mode is honestly labeled at the protocol level, not a hidden fallback."*

**One-sentence pitch for the "why does this matter" framing:** *ConcreSense screens fresh cement paste/mortar on-site against IS 456:2000 thresholds using a $5,000-BOM sensor-fusion device with on-device TinyML inference, GPS-tagging, and live MQTT dashboard logging — not yet validated against lab-tested concrete, but the full sensing→inference→cloud pipeline runs end-to-end tonight.*

---

## 2. Prior art / market context (for a "why this matters" slide)

Real, cited data points — nothing here is invented:

- **The standard on-site test today is the slump cone (ASTM C143 / IS equivalent)**: a purely manual, single-parameter (workability only) test that must be completed within **2.5 minutes** of sampling, using a $10 metal cone. It tells you nothing about temperature, vibration/compaction quality, or moisture ratio directly — three of the four channels ConcreSense fuses.
- **Commercial rapid/rugged concrete-quality devices already exist and validate the market need**, but they target *hardening/cured* concrete, not the fresh, pre-pour screening window ConcreSense targets:
  - **Giatec SmartRock2** — a wireless embedded maturity sensor that tracks concrete temperature from fresh through hardening to estimate in-place strength.
  - **Giatec Surf** — a handheld surface-resistivity meter, "8 measurements in under 15 seconds," used for concrete quality/durability QC.
- **Academic direction backs the sensor-fusion + edge-inference approach**: recent literature (2025) explicitly discusses low-cost embedded sensor platforms for concrete monitoring (ESP32 + humidity/temp sensors embedded in curing cylinders) and TinyML-based on-device inference for civil-infrastructure monitoring — i.e., "cheap MCU + fused sensors + on-device ML for concrete/structural assessment" is an active, credible research direction, not a fringe idea.
- **Could not verify:** any published number for "time saved" or "cost saved" versus lab cylinder-break testing for a device like this — don't cite a percentage improvement, since no source substantiates one. Stick to the qualitative gap (slump cone = single parameter, minutes, no data logging; ConcreSense = multi-parameter, GPS-tagged, cloud-logged).

---

## 3. GPS-no-fix-indoors — the crisp technical answer

If asked "why doesn't the GPS work in this room," this is the confident one-liner:

> *"A GPS satellite's signal arrives at the ground at about **−130 dBm** under clear sky. The NEO-6M needs about **−147 dBm** just to acquire a fix from cold start — so outdoors there's only about a 17 dB margin to begin with. A single external wall or roof already costs roughly **6–13 dB** of attenuation depending on material, and indoors you also lose most of the open sky the receiver needs to see multiple satellites at once. That easily eats the entire margin, which is why it's a physics limit of the module, not a wiring or firmware bug."*

Backing numbers (all from u-blox NEO-6 datasheet + standard RF-propagation references):
- Typical GPS signal at Earth's surface, clear line of sight: **≈ −130 dBm**.
- NEO-6M cold-start acquisition sensitivity: **≈ −147 dBm** (tracking sensitivity is better, ≈ −161 dBm, but that's *after* an initial fix — irrelevant indoors, since cold start is what fails).
- Typical building-material attenuation: plasterboard ≈ 3 dB, cinder block ≈ 4 dB, glass-with-metal-frame ≈ 6 dB, exterior wall ≈ 8 dB, metal door in brick ≈ 12.4 dB, per-floor penetration in multi-story buildings ≈ 17 dB.
- NEO-6M cold-start time is ~27 s minimum even with a visible sky — another reason a quick indoor demo won't catch a fix even near a window.

This matches what's already documented in this repo's own audit (`AUDIT.md` §F.3: "NEO-6M gets no fix indoors, and cold start is 27 s+").

---

## 4. Reminders (from this repo's own honesty constraints)

- Never say "accuracy" for the TinyML numbers unqualified — say **"synthetic-set separability."**
- Every generated training row is tagged `data_source=synthetic_physics` — you can point to this if asked how you avoided overclaiming.
- Don't touch wiring tonight — whatever's on the breadboard now is what you demo. Use `sim` mode to complete the pipeline story for moisture/vibration without touching a jumper.
- If a judge pushes on "what did you validate against," the honest answer that survives follow-up: *"Physics-simulated dataset, honestly labelled; real-concrete cylinder validation is the explicit next step."* That's a stronger answer than any claim that could unravel under a second question.

---

### Sources consulted
- u-blox NEO-6 Product Summary / Data Sheet (sensitivity, cold-start time)
- ASTM C143 slump test procedure references (timing, method)
- Giatec SmartRock2 / Giatec Surf product pages (commercial rapid concrete QC devices)
- MDPI *Applied Sciences* (2025), "Assessment of Low-Cost Sensors in Early-Age Concrete"; *Eng* journal (2025), low-cost ESP32-based embedded sensing for concrete hydration monitoring
- ResearchGate, "TinyML-enabled structural health monitoring for real-time anomaly detection in civil infrastructure" (2025)
- General RF indoor-attenuation references (Digi indoor path-loss note; ITU indoor attenuation model)
- Capstone/hackathon judging guidance (MSU ECE 480 judging criteria; UC Berkeley MICS capstone judging guidelines; general hackathon-advice sources)
- This repo's own `AUDIT.md` — ground truth for what's honestly claimable tonight
