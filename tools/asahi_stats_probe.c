// Minimal probe for DRM_IOCTL_ASAHI_GET_STATS (Asahi Linux / Apple AGX).
// Build: cc -o stats stats.c   Run: ./stats [/dev/dri/renderD128]
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DRM_COMMAND_BASE 0x40
#define DRM_IOCTL_ASAHI_GET_PARAMS _IOW(0x64, 0x40 + 0, struct get_params)  /* 'd' */
#define DRM_IOCTL_ASAHI_GET_STATS  _IOWR(0x64, 0x40 + 11, struct get_stats)

struct get_params { uint32_t param_group, pad; uint64_t pointer, size; };

struct params_global {
  uint64_t features;
  uint32_t gpu_generation, gpu_variant, gpu_revision, chip_id;
  uint32_t num_dies, num_clusters_total, num_cores_per_cluster, max_frequency_khz;
};

struct get_stats {
  uint64_t flags;
  uint64_t timestamp_ns;
  uint32_t valid;
  uint32_t busy_percent;
  uint64_t gpu_busy_ns, gpu_idle_ns;
  uint32_t pstate, frequency_khz;
  uint64_t power_uw;
  int32_t temp_mcelsius;
  uint32_t pad;
};

#define STAT_BUSY   (1u << 0)
#define STAT_PSTATE (1u << 1)
#define STAT_FREQ   (1u << 2)
#define STAT_POWER  (1u << 3)
#define STAT_TEMP   (1u << 4)

int main(int argc, char** argv) {
  const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
  int fd = open(node, O_RDWR | O_CLOEXEC);
  if (fd < 0) { fprintf(stderr, "open %s: %s\n", node, strerror(errno)); return 1; }

  struct params_global p; memset(&p, 0, sizeof p);
  struct get_params gp = { .param_group = 0, .pad = 0, .pointer = (uint64_t)(uintptr_t)&p, .size = sizeof p };
  if (ioctl(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &gp) == 0) {
    printf("GET_PARAMS ok: chip T%04u, AGX G%u%c, %u clusters x %u cores, max %u MHz\n",
      p.chip_id, p.gpu_generation, (char)p.gpu_variant,
      p.num_clusters_total, p.num_cores_per_cluster, p.max_frequency_khz / 1000);
  } else {
    printf("GET_PARAMS failed: %s\n", strerror(errno));
  }

  for (int i = 0; i < 3; i++) {
    struct get_stats s; memset(&s, 0, sizeof s);
    if (ioctl(fd, DRM_IOCTL_ASAHI_GET_STATS, &s) != 0) {
      printf("GET_STATS failed: %s (kernel lacks DRM_ASAHI_GET_STATS)\n", strerror(errno));
      return 1;
    }
    printf("GET_STATS: valid=0x%x busy=%u%% window[%.3f s busy / %.3f s idle] pstate=%u freq=%u MHz power=%.1f mW temp=%.1f C\n",
      s.valid, s.busy_percent,
      (double)s.gpu_busy_ns / 1e9, (double)s.gpu_idle_ns / 1e9,
      (s.valid & STAT_PSTATE) ? s.pstate : 0,
      (s.valid & STAT_FREQ) ? s.frequency_khz / 1000 : 0,
      (s.valid & STAT_POWER) ? (double)s.power_uw / 1000.0 : 0.0,
      (s.valid & STAT_TEMP) ? (double)s.temp_mcelsius / 1000.0 : 0.0);
    for (volatile int spin = 0; spin < 200000000; spin++);
  }
  return 0;
}
