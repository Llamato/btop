# Apple AGX iGPU on Asahi Linux

Asahi Linux runs mainline Linux on Apple silicon. The GPU is driven by the
[`asahi`](https://github.com/AsahiLinux/linux/tree/asahi/drivers/gpu/drm/asahi) DRM driver (Rust, in
the Asahi kernel tree and in mainline as of 6.13+ for display-only use). btop uses it for the GPU
box on those machines, selected by the `apple` keyword in `shown_gpus`.

There is no vendor library to talk to (no NVML/ROCm/AMF equivalent), everything comes from the asahi
DRM driver and the Apple SMC hwmon sensors.

## What works with a stock Asahi kernel

`Gpu::Asahi::init()` scans `/sys/class/drm/cardN`, keeps the cards whose `device/driver` symlink is
`asahi`, and identifies them with `DRM_IOCTL_ASAHI_GET_PARAMS`:

| Field | Source |
| --- | --- |
| SoC name (`Apple M1 Max`) | `drm_asahi_params_global.chip_id` (BCD), mapped through `Cpu::apple_cpu_id_to_name()`; falls back to the `apple,agx-tNNNN` compatible string of the OF node |
| GPU model (`AGX G13C`) | `gpu_generation` + `gpu_variant` |
| Cores / clusters, max clock | `num_clusters_total`, `num_cores_per_cluster`, `max_frequency_khz` |
| Temperature | first hwmon sensor labelled `*GPU*` (macsmc_hwmon), if the device tree exposes one |

Those machines show a GPU box with name, temperature (when the SMC exposes a GPU sensor) and the
identity line in the log. With neither temperature nor the stats ioctl below, the box would be empty,
so the GPU is skipped instead and an informational message is logged.

## Dynamic stats: `DRM_IOCTL_ASAHI_GET_STATS`

The asahi driver publishes no utilisation, clock or power value in sysfs and its UAPI has no stats
ioctl, so utilization/clock come from `DRM_IOCTL_ASAHI_GET_STATS`, an extension to the driver:

```c
/* enum drm_asahi_ioctl_id, appended after DRM_ASAHI_SUBMIT */
DRM_ASAHI_GET_STATS,     /* DRM_IOCTL_ASAHI(WR, GET_STATS, get_stats) */

#define DRM_ASAHI_STAT_BUSY    (1 << 0)
#define DRM_ASAHI_STAT_PSTATE  (1 << 1)
#define DRM_ASAHI_STAT_FREQ    (1 << 2)
#define DRM_ASAHI_STAT_POWER   (1 << 3)
#define DRM_ASAHI_STAT_TEMP    (1 << 4)

struct drm_asahi_get_stats {
    __u64 flags;          /* MBZ */
    __u64 timestamp_ns;   /* CLOCK_MONOTONIC of the sample */
    __u32 valid;          /* DRM_ASAHI_STAT_xxx */
    __u32 busy_percent;   /* utilisation of the previous window */
    __u64 gpu_busy_ns;    /* totals since driver init */
    __u64 gpu_idle_ns;
    __u32 pstate;
    __u32 frequency_khz;
    __u64 power_uw;
    __s32 temp_mcelsius;
    __u32 pad;
};
```

Semantics, as implemented by the kernel side:

* `busy_percent` is the fraction of time the driver had submissions in flight since the previous
  `GET_STATS` call on that device. The window is per device and shared by all clients, so a second
  client polling the GPU shortens the window seen by the first one. `gpu_busy_ns` / `gpu_idle_ns`
  are running totals since probe and can be used to build windows of any length instead.
* `pstate` is the performance state last reported by the firmware stats channel, `frequency_khz` is
  the core clock of that state, resolved from the operating points in the device tree. btop draws it
  as `P-state: P<n>` (the same slot nvidia cards use) and the clock as the GPU clock.
* `power_uw` and `temp_mcelsius` are defined but never flagged valid by the current kernel
  implementation: the firmware does send power/temperature messages, but their units are not
  understood well enough to publish them. btop therefore keeps using the SMC sensor for temperature
  and leaves power unsupported.
* The ioctl is render-node accessible (`DRM_RENDER_ALLOW`, no `DRM_AUTH`), so btop can query
  `/dev/dri/renderD128` without DRM master.

Kernels without the extension fail the ioctl with `EINVAL` (the DRM core rejects unknown Asahi
command numbers), which btop treats as "those functions are unsupported" rather than an error.

## Checking whether your kernel supports it

Run btop with `debug=true` (or `btop --debug`) and look for `asahi:` in `~/.config/btop/btop.log`,
or use a standalone probe:

```c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

struct get_params { uint32_t param_group, pad; uint64_t pointer, size; };
struct get_stats {
    uint64_t flags, timestamp_ns;
    uint32_t valid, busy_percent;
    uint64_t gpu_busy_ns, gpu_idle_ns;
    uint32_t pstate, frequency_khz;
    uint64_t power_uw;
    int32_t temp_mcelsius;
    uint32_t pad;
};

/* 'd' = DRM_IOCTL_BASE */
#define GET_PARAMS _IOW(0x64, 0x40 + 0, struct get_params)
#define GET_STATS  _IOWR(0x64, 0x40 + 11, struct get_stats)

int main(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct get_stats s; memset(&s, 0, sizeof s);
    if (ioctl(fd, GET_STATS, &s)) { printf("no GET_STATS: %s\n", strerror(errno)); return 1; }
    printf("busy=%u%% valid=0x%x freq=%u MHz\n", s.busy_percent, s.valid, s.frequency_khz / 1000);
    return 0;
}
```

## Building a kernel with the extension

The kernel side lives in the Asahi kernel tree (`AsahiLinux/linux`, `drivers/gpu/drm/asahi/`) as a
single commit, `drm/asahi: Add DRM_ASAHI_GET_STATS`, touching:

| File | Change |
| --- | --- |
| `include/uapi/drm/asahi_drm.h` | ioctl id, `DRM_ASAHI_STAT_xxx` flags, `struct drm_asahi_get_stats` |
| `drivers/gpu/drm/asahi/channel.rs` | `StatsState` (busy/idle counters, pstate) shared with `StatsChannel`, which records `PowerState` messages instead of only debug-logging them |
| `drivers/gpu/drm/asahi/gpu.rs` | `stats` field in `GpuManager`, `op_start()`/`op_end()` hooks in `start_op()`/`end_op()`, `stats_snapshot()` mapping pstate to a clock rate |
| `drivers/gpu/drm/asahi/file.rs` | `get_stats()` ioctl handler |
| `drivers/gpu/drm/asahi/driver.rs` | ioctl table entry (`RENDER_ALLOW`, no `AUTH`) |

On a NixOS machine using a custom kernel package, add it to that package's own `kernelPatches`
list:

```nix
kernelPatches = [ { name = "asahi-get-stats"; patch = ../patches/asahi-get-stats.patch; } ];
```

Do not put it in `boot.kernelPatches` when the kernel package already forwards
`config.boot.kernelPatches` into its `kernelPatches`: nixpkgs appends `boot.kernelPatches` to the
kernel derivation as well, and the patch then gets applied twice, which fails the build with
`Reversed (or previously applied) patch detected`.

To check a build by hand (Rust support needs the stdlib sources, which the nixpkgs `rustc` package
does not ship):

```sh
export RUST_LIB_SRC=/path/to/rust-src/lib/rustlib/src/rust/library
make olddefconfig && make -j$(nproc) vmlinux drivers/gpu/drm/asahi/asahi.ko
```

The kernel build also acts as the review gate for the patch: `clippy` (the kernel builds the Rust
DRM code with `-Wclippy::all`) and `perl scripts/kernel-doc -Wall include/uapi/drm/asahi_drm.h`
should stay quiet for the new code. Note that 128-bit division is not linked into modules
(`modpost: "__udivti3" undefined`), so the utilisation percentage must stay in 64-bit arithmetic.

After booting the patched kernel, `btop` picks the dynamic values up by itself; the probe program
above prints them too:

```
busy=42% valid=0xb freq=1296 MHz
```

## Known limitations

* Utilisation is *driver submissions in flight*, not AGX core occupancy. A single busy submission
  reports 100% even if only one of 32 cores is used, and short submissions between two polls can be
  missed. It is the only signal the driver has without firmware support.
* No memory usage: AGX shares system memory with the CPU and the driver exposes no per-client
  allocation accounting, so `mem_used`/`mem_total`/`mem_utilization` stay unsupported.
* Temperature depends on the SMC sensor set in the device tree. Some machines (e.g. an M1 Max with
  the upstream `t8103` DT) expose no sensor labelled `GPU`, in which case only the ioctl value could
  provide one.
* Encoder/decoder utilisation and PCIe counters are not available.
