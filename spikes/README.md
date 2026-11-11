# Spike evidence branch

This directory is retained executable Phase-0 evidence for the long-lived
`spikes` branch. `main` keeps canonical policy and verdict documentation only;
spike product code must never be merged there.

Each direct spike directory contains `README.md`, `spike.json`, and
`CMakeLists.txt`. Completed or closed spikes also retain `results/summary.json`.
The manifest records reproducible commands, provenance, dependencies, gates,
and verdict; large models and binaries stay out of Git and are recorded by
URL, version, hash, license, and acquisition command.

Create evidence as active, record results as completed or closed, and preserve
its final verdict. Parallel code work requires separate worktrees and exclusive
file ownership. Hardware/GPU/virtual-camera/Frame Server/OBS/meeting-app
measurement is serialized by one integration owner; use at most five agents.

The original A/A2 and B probes are missing. They can only be restored as
clearly labeled reconstructed probes, never represented as the original
measurements.
