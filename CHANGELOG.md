# Changelog

Each version's section is also its release notes on GitHub: the Packages
workflow publishes it when a `v<version>` tag is pushed.

## 0.1.0 (2026-09-26)

The first release of Culprit, a Linux system monitor built for diagnosis. It
shows what a task manager shows. It also tells you why the machine is loaded,
hot or stuttering, and which process, IRQ, device or kernel subsystem is
responsible.

![Overview tab](https://raw.githubusercontent.com/ProjectHax/culprit/v0.1.0/docs/screenshots/overview.png)

### Highlights

- **Load:** splits the load average into runnable and blocked (D-state)
  threads, and groups blocked threads by process and kernel wait channel. CPU
  oversubscription is reported with the processes causing it and the ones
  waiting, using run-queue wait measured per process.
- **Heat and power:** attributes CPU package power (RAPL) and NVIDIA GPU power
  to processes, and reads every hwmon sensor. It reports thermal limits, clock
  drops, GPU throttle reasons and stopped fans. Temperature channels that are
  probably not connected are marked with `*`.
- **Micro-stutter:** latency probes and a 50 Hz flight recorder catch each
  hitch and classify it: CPU contention, IRQ or softirq storms, memory
  reclaim, system-wide stalls, or stutter that repeats on a fixed period.
  Each hitch comes with ranked suspects.
- **Deep trace:** an optional root helper, started through polkit, traces the
  scheduler with `perf_event_open`. It names exactly which task or interrupt
  held the CPU during each stall.
- **Processes:** a tree or flat view with run-queue wait, preemptions, major
  faults, GPU use and estimated watts. It can terminate processes and set
  nice, I/O priority and CPU affinity for every thread.
- **Recordings and reports:** a headless recorder (`culprit --record`, or the
  `culprit-recorder` systemd user service), a Recordings tab to replay
  recordings, and text or JSON reports.
- **Look and feel:** light and dark themes that follow the desktop, and an
  integrated title bar that scales with the UI on GNOME/Wayland.

### Packages

| File | For | Install |
|---|---|---|
| `culprit_0.1.0_amd64.deb` | Ubuntu 22.04 or newer, Debian 12 or newer | `sudo apt install ./culprit_0.1.0_amd64.deb` |
| `culprit-0.1.0-1.el9.x86_64.rpm` | RHEL, AlmaLinux, Rocky Linux 9 and 10 | `sudo dnf install ./culprit-0.1.0-1.el9.x86_64.rpm` |
| `Culprit-0.1.0-x86_64.AppImage` | other x86-64 distributions with glibc 2.34 or newer | `chmod +x Culprit-0.1.0-x86_64.AppImage && ./Culprit-0.1.0-x86_64.AppImage` |

`SHA256SUMS` lists the checksums of all three.

- The .deb and the AppImage include Qt 6.10.3. The RPM uses your
  distribution's Qt, which must be 6.6 or newer.
- On EL9, Qt 6 comes from EPEL. Run `sudo dnf install epel-release` first.
- The AppImage needs FUSE. Without it, run the AppImage with
  `--appimage-extract-and-run`.

### Known limitations

- GPU metrics are NVIDIA only (NVML).
- Some distributions, including RHEL, turn off pressure stall information
  (PSI) at boot. To see CPU, memory and I/O pressure there, add `psi=1` to
  the kernel command line.
- Power attribution and hitch analysis are estimates. Deep trace gives the
  exact answer for stalls.

Culprit is Copyright (C) 2026 ProjectHax LLC and licensed under the GPL,
version 3 or later. Third-party licenses are listed in
`THIRD-PARTY-NOTICES.md`, in the About dialog, and in `THIRD-PARTY-BUNDLED.txt`
in the .deb and the AppImage.
