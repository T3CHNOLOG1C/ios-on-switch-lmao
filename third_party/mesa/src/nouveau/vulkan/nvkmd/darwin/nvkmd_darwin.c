/*
 * Copyright © 2026 IOSwitch
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_darwin.h"

#include "nv_device_info.h"
#include "util/bitscan.h"
#include "util/cache_ops.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <errno.h>
#include <inttypes.h>
#include <libkern/OSCacheControl.h>
#include <os/log.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Kernel waits are bounded; "forever" becomes this, after which the kernel
 * declares the channel dead (CONTRACT.md method 9).
 */
#define NVKMD_DARWIN_FOREVER_NS UINT64_MAX

/*
 * Transport
 */

static kern_return_t
darwin_call(const struct nvkmd_darwin_conn *c, uint32_t method,
            const void *in, size_t in_size, void *out, size_t out_size)
{
   size_t out_bytes = out_size;
   if (!c->tunnel) {
      return IOConnectCallStructMethod(c->conn, method, in, in_size,
                                       out_size ? out : NULL,
                                       out_size ? &out_bytes : NULL);
   }

   /* Sandboxed callers reach the driver through the one selector the
    * profile admits: {magic, method} then the record (CONTRACT.md §1).
    */
   uint32_t buffer[1024];
   if (in_size > sizeof(buffer) - 8)
      return kIOReturnBadArgument;
   buffer[0] = TegraGM20BTunnelMagic;
   buffer[1] = method;
   if (in_size)
      memcpy(&buffer[2], in, in_size);
   return IOConnectCallStructMethod(c->conn, c->selector, buffer, 8 + in_size,
                                    out_size ? out : NULL,
                                    out_size ? &out_bytes : NULL);
}

static bool
darwin_probe_tunnel(struct nvkmd_darwin_conn *c)
{
   const uint32_t header[2] = { TegraGM20BTunnelMagic, TegraGM20BTunnelProbe };
   for (uint32_t s = 0; s < 256; s++) {
      if (IOConnectCallStructMethod(c->conn, s, header, sizeof(header),
                                    NULL, NULL) == KERN_SUCCESS) {
         c->tunnel = true;
         c->selector = s;
         return true;
      }
   }
   return false;
}

static void
darwin_close(struct nvkmd_darwin_conn *c)
{
   if (c->conn != IO_OBJECT_NULL)
      IOServiceClose(c->conn);
   c->conn = IO_OBJECT_NULL;
}

/* Opens a connection (one GPU address space) and reads the device facts. */
static VkResult
darwin_open(struct vk_object_base *log_obj, struct nvkmd_darwin_conn *c,
            struct TegraGM20BAbiInfo *info)
{
   /* NVK_DARWIN_SERVICE names the IOKit class to open: by default the
    * accelerator the original Metal discovery finds.
    */
   const char *name = getenv("NVK_DARWIN_SERVICE");
   if (name == NULL || *name == '\0')
      name = "IOAcceleratorES";

   io_service_t service =
      IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
   if (service == IO_OBJECT_NULL)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   *c = (struct nvkmd_darwin_conn) { .conn = IO_OBJECT_NULL };
   kern_return_t kr = IOServiceOpen(service, mach_task_self(),
                                    TegraGM20BOpenType, &c->conn);
   IOObjectRelease(service);
   if (kr != KERN_SUCCESS)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   kr = darwin_call(c, TegraGM20BMethodGetInfo, NULL, 0, info, sizeof(*info));
   if (kr != KERN_SUCCESS && darwin_probe_tunnel(c))
      kr = darwin_call(c, TegraGM20BMethodGetInfo, NULL, 0, info, sizeof(*info));

   if (kr != KERN_SUCCESS || info->abi_version != TegraGM20BAbiVersion) {
      darwin_close(c);
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "TegraGM20B GetInfo failed (0x%x, ABI %u)",
                       kr, kr == KERN_SUCCESS ? info->abi_version : 0);
   }
   c->kernel_map = (info->flags & TegraGM20BInfoKernelMap) != 0;

   return VK_SUCCESS;
}

/* Maps the connection's memory of `token` into this task, at `fixed`
 * (kIOMapOverwrite) or anywhere (0): through the kext's Map method where it
 * has one, as Apple's GPU drivers map their resources from their external
 * methods, since a sandbox may admit io_connect_method and refuse the MIG
 * routine io_connect_map_memory_into_task behind IOConnectMapMemory64
 * (WebKit's GPU process in 21H461 does); else through IOConnectMapMemory64.
 * The same kernel work either way.
 */
static kern_return_t
darwin_map(const struct nvkmd_darwin_conn *c, uint32_t token, bool read_only,
           mach_vm_address_t fixed, mach_vm_address_t *addr,
           mach_vm_size_t *size)
{
   if (c->kernel_map) {
      const struct TegraGM20BAbiMap req = {
         .token = token,
         .flags = (read_only ? TegraGM20BMapReadOnly : 0) |
                  (fixed ? TegraGM20BMapFixed : 0),
         .address = fixed,
      };
      struct TegraGM20BAbiMapReply reply = { 0 };
      kern_return_t kr = darwin_call(c, TegraGM20BMethodMap, &req, sizeof(req),
                                     &reply, sizeof(reply));
      if (kr == KERN_SUCCESS) {
         *addr = reply.address;
         *size = reply.bytes;
      }
      return kr;
   }

   IOOptionBits options = fixed ? kIOMapOverwrite : kIOMapAnywhere;
   if (read_only)
      options |= kIOMapReadOnly;
   *addr = fixed;
   *size = 0;
   return IOConnectMapMemory64(c->conn, token, mach_task_self(), addr, size,
                               options);
}

/* Removes the mapping darwin_map made of `token` at `addr`. */
static void
darwin_unmap(const struct nvkmd_darwin_conn *c, uint32_t token,
             mach_vm_address_t addr)
{
   if (c->kernel_map) {
      const struct TegraGM20BAbiUnmap req = { .token = token, .address = addr };
      darwin_call(c, TegraGM20BMethodUnmap, &req, sizeof(req), NULL, 0);
      return;
   }
   IOConnectUnmapMemory64(c->conn, token, mach_task_self(), addr);
}

static VkResult
darwin_result(struct vk_object_base *log_obj, kern_return_t kr,
              const char *what)
{
   switch (kr) {
   case KERN_SUCCESS:
      return VK_SUCCESS;
   case kIOReturnNoMemory:
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%s", what);
   case kIOReturnNotResponding:
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST, "%s: channel dead", what);
   case kIOReturnTimeout:
      return VK_TIMEOUT;
   default:
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN, "%s failed: 0x%x", what, kr);
   }
}

/*
 * Fences and sync objects
 */

/* One process-wide lock and condition for sync state changes, so a waiter on
 * a not-yet-submitted binary sync wakes when a submission resolves it.
 */
static pthread_mutex_t darwin_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t darwin_sync_cond = PTHREAD_COND_INITIALIZER;

static inline bool
seqno_passed(uint32_t completed, uint32_t seqno)
{
   return (int32_t)(completed - seqno) >= 0;
}

static struct nvkmd_darwin_channel *
darwin_channel_locked(struct nvkmd_darwin_dev *dev, uint32_t id)
{
   for (unsigned i = 0; i < NVKMD_DARWIN_MAX_CHANNELS; i++) {
      if (dev->channels[i].id == id)
         return &dev->channels[i];
   }
   return NULL;
}

/* Waits until channel's completed seqno passes seqno or abs_timeout_ns. */
static VkResult
darwin_wait_seqno(struct nvkmd_darwin_dev *dev, uint32_t channel,
                  uint32_t seqno, uint64_t abs_timeout_ns)
{
   simple_mtx_lock(&dev->channels_mutex);
   struct nvkmd_darwin_channel *chan = darwin_channel_locked(dev, channel);
   const bool done = chan == NULL || seqno_passed(*chan->fence, seqno);
   simple_mtx_unlock(&dev->channels_mutex);

   /* A destroyed channel went idle before it was torn down. */
   if (done)
      return VK_SUCCESS;

   const uint64_t now = os_time_get_nano();
   struct TegraGM20BAbiWaitFence wait = {
      .channel = channel,
      .seqno = seqno,
      .timeout_ns = abs_timeout_ns == UINT64_MAX ? NVKMD_DARWIN_FOREVER_NS :
                    abs_timeout_ns > now ? abs_timeout_ns - now : 0,
   };
   kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodWaitFence,
                                  &wait, sizeof(wait), NULL, 0);
   if (kr == kIOReturnTimeout)
      return VK_TIMEOUT;
   return darwin_result(NULL, kr, "WaitFence");
}

static struct nvkmd_darwin_sync *
to_darwin_sync(struct vk_sync *sync)
{
   return container_of(sync, struct nvkmd_darwin_sync, base);
}

static VkResult
darwin_sync_init(struct vk_device *device, struct vk_sync *sync,
                 uint64_t initial_value)
{
   struct nvkmd_darwin_sync *s = to_darwin_sync(sync);
   s->state = initial_value ? NVKMD_DARWIN_SYNC_SIGNALED :
                              NVKMD_DARWIN_SYNC_UNSIGNALED;
   s->dev = NULL;
   s->channel = 0;
   s->seqno = 0;
   s->value = initial_value;
   util_dynarray_init(&s->points, NULL);
   return VK_SUCCESS;
}

static void
darwin_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   util_dynarray_fini(&to_darwin_sync(sync)->points);
}

static bool
darwin_is_timeline(const struct nvkmd_darwin_sync *s)
{
   return (s->base.flags & VK_SYNC_IS_TIMELINE) != 0;
}

/* Whether the GPU has passed a timeline point (a destroyed channel went idle
 * before it was torn down).
 */
static bool
darwin_point_done(const struct nvkmd_darwin_sync_point *p)
{
   if (p->dev == NULL)
      return true;
   simple_mtx_lock(&p->dev->channels_mutex);
   struct nvkmd_darwin_channel *chan = darwin_channel_locked(p->dev, p->channel);
   const bool done = chan == NULL || seqno_passed(*chan->fence, p->seqno);
   simple_mtx_unlock(&p->dev->channels_mutex);
   return done;
}

/* darwin_sync_lock held: fold every point the GPU has passed into value, and
 * drop every point at or below it.
 */
static void
darwin_timeline_update_locked(struct nvkmd_darwin_sync *s)
{
   struct nvkmd_darwin_sync_point *points = util_dynarray_begin(&s->points);
   unsigned count = util_dynarray_num_elements(&s->points,
                                               struct nvkmd_darwin_sync_point);
   for (unsigned i = 0; i < count; i++) {
      if (points[i].value > s->value && darwin_point_done(&points[i]))
         s->value = points[i].value;
   }
   unsigned kept = 0;
   for (unsigned i = 0; i < count; i++) {
      if (points[i].value > s->value)
         points[kept++] = points[i];
   }
   s->points.size = kept * sizeof(*points);
}

/* darwin_sync_lock held: the earliest submitted point that reaches value. */
static bool
darwin_timeline_point_locked(struct nvkmd_darwin_sync *s, uint64_t value,
                             struct nvkmd_darwin_sync_point *out)
{
   bool found = false;
   util_dynarray_foreach(&s->points, struct nvkmd_darwin_sync_point, p) {
      if (p->value >= value && (!found || p->value < out->value)) {
         *out = *p;
         found = true;
      }
   }
   return found;
}

/* A timeline signal: by the CPU (dev NULL) or pending on channel/seqno. */
static void
darwin_timeline_signal(struct nvkmd_darwin_sync *s, uint64_t value,
                       struct nvkmd_darwin_dev *dev, uint32_t channel,
                       uint32_t seqno)
{
   pthread_mutex_lock(&darwin_sync_lock);
   if (dev == NULL) {
      if (value > s->value)
         s->value = value;
   } else if (value > s->value) {
      const struct nvkmd_darwin_sync_point point = {
         .value = value, .dev = dev, .channel = channel, .seqno = seqno,
      };
      util_dynarray_append_typed(&s->points, struct nvkmd_darwin_sync_point, point);
   }
   darwin_timeline_update_locked(s);
   pthread_cond_broadcast(&darwin_sync_cond);
   pthread_mutex_unlock(&darwin_sync_lock);
}

static VkResult
darwin_sync_get_value(struct vk_device *device, struct vk_sync *sync,
                      uint64_t *value)
{
   struct nvkmd_darwin_sync *s = to_darwin_sync(sync);
   pthread_mutex_lock(&darwin_sync_lock);
   darwin_timeline_update_locked(s);
   *value = s->value;
   pthread_mutex_unlock(&darwin_sync_lock);
   return VK_SUCCESS;
}

static void
darwin_sync_set(struct nvkmd_darwin_sync *s, enum nvkmd_darwin_sync_state state,
                struct nvkmd_darwin_dev *dev, uint32_t channel, uint32_t seqno)
{
   pthread_mutex_lock(&darwin_sync_lock);
   s->state = state;
   s->dev = dev;
   s->channel = channel;
   s->seqno = seqno;
   pthread_cond_broadcast(&darwin_sync_cond);
   pthread_mutex_unlock(&darwin_sync_lock);
}

static VkResult
darwin_sync_signal(struct vk_device *device, struct vk_sync *sync,
                   uint64_t value)
{
   struct nvkmd_darwin_sync *s = to_darwin_sync(sync);
   if (darwin_is_timeline(s))
      darwin_timeline_signal(s, value, NULL, 0, 0);
   else
      darwin_sync_set(s, NVKMD_DARWIN_SYNC_SIGNALED, NULL, 0, 0);
   return VK_SUCCESS;
}

static VkResult
darwin_sync_reset(struct vk_device *device, struct vk_sync *sync)
{
   darwin_sync_set(to_darwin_sync(sync), NVKMD_DARWIN_SYNC_UNSIGNALED, NULL, 0, 0);
   return VK_SUCCESS;
}

static VkResult
darwin_sync_move(struct vk_device *device, struct vk_sync *dst,
                 struct vk_sync *src)
{
   struct nvkmd_darwin_sync *d = to_darwin_sync(dst);
   struct nvkmd_darwin_sync *s = to_darwin_sync(src);

   pthread_mutex_lock(&darwin_sync_lock);
   d->state = s->state;
   d->dev = s->dev;
   d->channel = s->channel;
   d->seqno = s->seqno;
   s->state = NVKMD_DARWIN_SYNC_UNSIGNALED;
   s->dev = NULL;
   pthread_cond_broadcast(&darwin_sync_cond);
   pthread_mutex_unlock(&darwin_sync_lock);
   return VK_SUCCESS;
}

/* darwin_sync_lock held: sleeps until a sync changes or abs_timeout_ns.
 * Darwin condition variables wait on the realtime clock; deadlines are
 * monotonic.
 */
static int
darwin_sync_cond_wait_locked(uint64_t abs_timeout_ns)
{
   if (abs_timeout_ns == UINT64_MAX)
      return pthread_cond_wait(&darwin_sync_cond, &darwin_sync_lock);
   const uint64_t now = os_time_get_nano();
   if (abs_timeout_ns <= now)
      return ETIMEDOUT;
   const uint64_t rel = abs_timeout_ns - now;
   struct timespec abstime;
   clock_gettime(CLOCK_REALTIME, &abstime);
   const uint64_t nsec = (uint64_t)abstime.tv_nsec + rel % 1000000000ull;
   abstime.tv_sec += rel / 1000000000ull + nsec / 1000000000ull;
   abstime.tv_nsec = nsec % 1000000000ull;
   return pthread_cond_timedwait(&darwin_sync_cond, &darwin_sync_lock, &abstime);
}

/* Waits for the sync to leave the unsignaled state (to be submitted). */
static VkResult
darwin_sync_wait_resolved(struct nvkmd_darwin_sync *s, uint64_t abs_timeout_ns)
{
   pthread_mutex_lock(&darwin_sync_lock);
   while (s->state == NVKMD_DARWIN_SYNC_UNSIGNALED) {
      if (darwin_sync_cond_wait_locked(abs_timeout_ns) == ETIMEDOUT) {
         pthread_mutex_unlock(&darwin_sync_lock);
         return VK_TIMEOUT;
      }
   }
   pthread_mutex_unlock(&darwin_sync_lock);
   return VK_SUCCESS;
}

/* Waits until the timeline reaches value; with VK_SYNC_WAIT_PENDING, until a
 * signal operation that reaches it has been submitted. If same_channel is
 * nonzero, a point pending on that channel counts as reached (a channel
 * executes in order).
 */
static VkResult
darwin_timeline_wait(struct nvkmd_darwin_sync *s, uint64_t value,
                     enum vk_sync_wait_flags flags, uint32_t same_channel,
                     uint64_t abs_timeout_ns)
{
   pthread_mutex_lock(&darwin_sync_lock);
   for (;;) {
      darwin_timeline_update_locked(s);
      if (s->value >= value)
         break;
      struct nvkmd_darwin_sync_point point;
      if (!darwin_timeline_point_locked(s, value, &point)) {
         if (darwin_sync_cond_wait_locked(abs_timeout_ns) == ETIMEDOUT) {
            pthread_mutex_unlock(&darwin_sync_lock);
            return VK_TIMEOUT;
         }
         continue;
      }
      if ((flags & VK_SYNC_WAIT_PENDING) ||
          (same_channel != 0 && point.channel == same_channel))
         break;
      pthread_mutex_unlock(&darwin_sync_lock);
      VkResult result = darwin_wait_seqno(point.dev, point.channel,
                                          point.seqno, abs_timeout_ns);
      if (result != VK_SUCCESS)
         return result;
      pthread_mutex_lock(&darwin_sync_lock);
   }
   pthread_mutex_unlock(&darwin_sync_lock);
   return VK_SUCCESS;
}

static VkResult
darwin_sync_wait_one(struct nvkmd_darwin_sync *s, enum vk_sync_wait_flags flags,
                     uint64_t abs_timeout_ns)
{
   VkResult result = darwin_sync_wait_resolved(s, abs_timeout_ns);
   if (result != VK_SUCCESS || (flags & VK_SYNC_WAIT_PENDING))
      return result;

   pthread_mutex_lock(&darwin_sync_lock);
   const enum nvkmd_darwin_sync_state state = s->state;
   struct nvkmd_darwin_dev *dev = s->dev;
   const uint32_t channel = s->channel, seqno = s->seqno;
   pthread_mutex_unlock(&darwin_sync_lock);

   if (state != NVKMD_DARWIN_SYNC_FENCE)
      return VK_SUCCESS;

   return darwin_wait_seqno(dev, channel, seqno, abs_timeout_ns);
}

static VkResult
darwin_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                      const struct vk_sync_wait *waits,
                      enum vk_sync_wait_flags wait_flags,
                      uint64_t abs_timeout_ns)
{
   if (!(wait_flags & VK_SYNC_WAIT_ANY)) {
      for (uint32_t i = 0; i < wait_count; i++) {
         struct nvkmd_darwin_sync *s = to_darwin_sync(waits[i].sync);
         VkResult result = darwin_is_timeline(s) ?
            darwin_timeline_wait(s, waits[i].wait_value, wait_flags, 0,
                                 abs_timeout_ns) :
            darwin_sync_wait_one(s, wait_flags, abs_timeout_ns);
         if (result != VK_SUCCESS)
            return result;
      }
      return VK_SUCCESS;
   }

   /* Any: poll each in turn (no interrupts on this GPU anyway). */
   for (;;) {
      for (uint32_t i = 0; i < wait_count; i++) {
         struct nvkmd_darwin_sync *s = to_darwin_sync(waits[i].sync);
         VkResult result = darwin_is_timeline(s) ?
            darwin_timeline_wait(s, waits[i].wait_value, wait_flags, 0, 0) :
            darwin_sync_wait_one(s, wait_flags, 0);
         if (result != VK_TIMEOUT)
            return result;
      }
      if (abs_timeout_ns != UINT64_MAX && os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;
      usleep(100);
   }
}

static struct vk_sync_type
darwin_sync_type(void)
{
   return (struct vk_sync_type) {
      .size = sizeof(struct nvkmd_darwin_sync),
      .features = VK_SYNC_FEATURE_BINARY |
                  VK_SYNC_FEATURE_TIMELINE |
                  VK_SYNC_FEATURE_GPU_WAIT |
                  VK_SYNC_FEATURE_GPU_MULTI_WAIT |
                  VK_SYNC_FEATURE_CPU_WAIT |
                  VK_SYNC_FEATURE_CPU_RESET |
                  VK_SYNC_FEATURE_CPU_SIGNAL |
                  VK_SYNC_FEATURE_WAIT_ANY |
                  VK_SYNC_FEATURE_WAIT_PENDING,
      .init = darwin_sync_init,
      .finish = darwin_sync_finish,
      .signal = darwin_sync_signal,
      .reset = darwin_sync_reset,
      .move = darwin_sync_move,
      .get_value = darwin_sync_get_value,
      .wait_many = darwin_sync_wait_many,
   };
}

/*
 * Physical device
 */

static uint8_t
sm_for_chipset(uint16_t chipset)
{
   /* Maxwell and Pascal, as nouveau_device.c's table. */
   if (chipset >= 0x13b)
      return 62;
   else if (chipset >= 0x132)
      return 61;
   else if (chipset >= 0x130)
      return 60;
   else if (chipset >= 0x12b)
      return 53;
   else if (chipset >= 0x120)
      return 52;
   else if (chipset >= 0x110)
      return 50;
   return 0;
}

static void
nvkmd_darwin_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   FREE(nvkmd_darwin_pdev(_pdev));
}

static uint64_t
nvkmd_darwin_pdev_get_vram_used(struct nvkmd_pdev *pdev)
{
   return 0; /* Tegra: no VRAM */
}

static VkResult nvkmd_darwin_create_dev(struct nvkmd_pdev *pdev,
                                        struct vk_object_base *log_obj,
                                        struct nvkmd_dev **dev_out);

const struct nvkmd_pdev_ops nvkmd_darwin_pdev_ops = {
   .destroy = nvkmd_darwin_pdev_destroy,
   .get_vram_used = nvkmd_darwin_pdev_get_vram_used,
   .create_dev = nvkmd_darwin_create_dev,
};

VkResult
nvkmd_darwin_create_pdev(struct vk_object_base *log_obj,
                         enum nvk_debug debug_flags,
                         struct nvkmd_pdev **pdev_out)
{
   struct nvkmd_darwin_conn conn;
   struct TegraGM20BAbiInfo info = { 0 };
   VkResult result = darwin_open(log_obj, &conn, &info);
   if (result != VK_SUCCESS)
      return result;
   darwin_close(&conn);

   const uint8_t sm = sm_for_chipset(info.chipset);
   if (sm == 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct nvkmd_darwin_pdev *pdev = CALLOC_STRUCT(nvkmd_darwin_pdev);
   if (pdev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   pdev->info = info;
   pdev->base.ops = &nvkmd_darwin_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   struct nv_device_info *dev_info = &pdev->base.dev_info;
   dev_info->type = NV_DEVICE_TYPE_SOC;
   dev_info->device_id = info.device_id;
   dev_info->chipset = info.chipset;
   STATIC_ASSERT(sizeof(dev_info->device_name) >= sizeof(info.device_name));
   memcpy(dev_info->device_name, info.device_name, sizeof(info.device_name));
   dev_info->device_name[sizeof(dev_info->device_name) - 1] = '\0';
   STATIC_ASSERT(sizeof(dev_info->chipset_name) >= sizeof(info.chip_name));
   memcpy(dev_info->chipset_name, info.chip_name, sizeof(info.chip_name));
   dev_info->chipset_name[sizeof(dev_info->chipset_name) - 1] = '\0';

   dev_info->sm = sm;
   dev_info->gpc_count = info.gpc_count;
   dev_info->tpc_count = info.tpc_count;
   /* SM5x/SM6x values of nouveau_device.c's tables */
   dev_info->mp_per_tpc = info.chipset == 0x130 ? 2 : 1;
   dev_info->max_warps_per_mp = 64;
   dev_info->max_blocks_per_mp = 32;
   dev_info->sm_smem_sizes_kB[0] = (sm == 52 || sm == 61) ? 96 : 64;
   dev_info->sm_smem_size_count = 1;
   dev_info->max_smem_per_wg_kB = 48;

   dev_info->has_transfer_queue = false;
   dev_info->has_video = false;
   dev_info->nc_atom_size_B = util_cache_granularity();

   dev_info->cls_copy = info.cls_copy;
   dev_info->cls_eng2d = info.cls_eng2d;
   dev_info->cls_eng3d = info.cls_eng3d;
   dev_info->cls_m2mf = info.cls_m2mf;
   dev_info->cls_compute = info.cls_compute;
   dev_info->cls_gpfifo = info.cls_gpfifo;
   dev_info->vram_size_B = 0;
   dev_info->bar_size_B = 0;

   if (info.flags & TegraGM20BInfoZcull) {
      STATIC_ASSERT(sizeof(dev_info->zcull_info) == sizeof(info.zcull));
      memcpy(&dev_info->zcull_info, info.zcull, sizeof(info.zcull));
      dev_info->has_zcull_info = true;
   }

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_map_fixed = true,
      .has_overmap = true,
      .has_host_import = (info.flags & TegraGM20BInfoHostImport) != 0,
   };

   uint64_t os_page_size = 4096;
   os_get_page_size(&os_page_size);
   pdev->base.bind_align_B = MAX2(info.page_size, (uint32_t)os_page_size);

   /* One native type, binary and timeline: NVK's own timelines (nvk_mem_stream)
    * need sync_types[0] to be one. Without WAIT_BEFORE_SIGNAL the runtime
    * runs it in assisted mode (vk_device.c get_timeline_mode).
    */
   pdev->sync_type = darwin_sync_type();
   pdev->sync_types[0] = &pdev->sync_type;
   pdev->sync_types[1] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   /* NVK only exposes conformant (discrete) GPUs unless told otherwise; this
    * backend exists for a Tegra, so say so for it.
    */
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);

   *pdev_out = &pdev->base;
   return VK_SUCCESS;
}

/*
 * Device
 */

static void
nvkmd_darwin_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);

   util_vma_heap_finish(&dev->heap);
   util_vma_heap_finish(&dev->replay_heap);
   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->channels_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   darwin_close(&dev->conn);
   FREE(dev);
}

static uint64_t
nvkmd_darwin_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);
   struct TegraGM20BAbiTimestamp ts = { 0 };
   if (darwin_call(&dev->conn, TegraGM20BMethodGetTimestamp, NULL, 0,
                   &ts, sizeof(ts)) != KERN_SUCCESS)
      return 0;
   return ts.ns;
}

static VkResult nvkmd_darwin_alloc_mem(struct nvkmd_dev *dev,
                                       struct vk_object_base *log_obj,
                                       uint64_t size_B, uint64_t align_B,
                                       enum nvkmd_mem_flags flags,
                                       struct nvkmd_mem **mem_out);
static VkResult nvkmd_darwin_alloc_tiled_mem(struct nvkmd_dev *dev,
                                             struct vk_object_base *log_obj,
                                             uint64_t size_B, uint64_t align_B,
                                             uint8_t pte_kind, uint16_t tile_mode,
                                             enum nvkmd_mem_flags flags,
                                             struct nvkmd_mem **mem_out);
static VkResult nvkmd_darwin_import_dma_buf(struct nvkmd_dev *dev,
                                            struct vk_object_base *log_obj,
                                            int fd, struct nvkmd_mem **mem_out);
static VkResult nvkmd_darwin_import_host_mem(struct nvkmd_dev *dev,
                                             struct vk_object_base *log_obj,
                                             void *host_ptr, uint64_t size_B,
                                             struct nvkmd_mem **mem_out);
static VkResult nvkmd_darwin_alloc_va(struct nvkmd_dev *dev,
                                      struct vk_object_base *log_obj,
                                      enum nvkmd_va_flags flags, uint8_t pte_kind,
                                      uint64_t size_B, uint64_t align_B,
                                      uint64_t fixed_addr, struct nvkmd_va **va_out);
static VkResult nvkmd_darwin_create_ctx(struct nvkmd_dev *dev,
                                        struct vk_object_base *log_obj,
                                        enum nvkmd_engines engines,
                                        struct nvkmd_ctx **ctx_out);

const struct nvkmd_dev_ops nvkmd_darwin_dev_ops = {
   .destroy = nvkmd_darwin_dev_destroy,
   .get_gpu_timestamp = nvkmd_darwin_dev_get_gpu_timestamp,
   .alloc_mem = nvkmd_darwin_alloc_mem,
   .alloc_tiled_mem = nvkmd_darwin_alloc_tiled_mem,
   .import_dma_buf = nvkmd_darwin_import_dma_buf,
   .import_host_mem = nvkmd_darwin_import_host_mem,
   .alloc_va = nvkmd_darwin_alloc_va,
   .create_ctx = nvkmd_darwin_create_ctx,
};

static VkResult
nvkmd_darwin_create_dev(struct nvkmd_pdev *_pdev,
                        struct vk_object_base *log_obj,
                        struct nvkmd_dev **dev_out)
{
   struct nvkmd_darwin_pdev *pdev = nvkmd_darwin_pdev(_pdev);

   struct nvkmd_darwin_dev *dev = CALLOC_STRUCT(nvkmd_darwin_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Each device gets its own connection, hence its own GPU address space. */
   struct TegraGM20BAbiInfo info;
   VkResult result = darwin_open(log_obj, &dev->conn, &info);
   if (result != VK_SUCCESS) {
      FREE(dev);
      return result == VK_ERROR_INCOMPATIBLE_DRIVER ?
             vk_error(log_obj, VK_ERROR_INITIALIZATION_FAILED) : result;
   }

   dev->base.ops = &nvkmd_darwin_dev_ops;
   dev->base.pdev = &pdev->base;
   dev->base.va_start = 0;
   dev->base.va_end = info.va_end;
   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->channels_mutex, mtx_plain);

   /* Same split as nvkmd_nouveau: ordinary VAs below, capture/replay VAs in
    * the upper half of the client window.
    */
   const uint64_t start = MAX2(info.va_start, 4096);
   dev->replay_start = start + (info.va_end - start) / 2;
   simple_mtx_init(&dev->heap_mutex, mtx_plain);
   util_vma_heap_init(&dev->heap, start, dev->replay_start - start);
   util_vma_heap_init(&dev->replay_heap, dev->replay_start,
                      info.va_end - dev->replay_start);

   *dev_out = &dev->base;
   return VK_SUCCESS;
}

/*
 * Memory
 */

/* Gives a new BO its own VA, bound whole (as nvkmd_nouveau's
 * create_mem_or_close_bo).
 */
static VkResult
darwin_mem_finish_create(struct nvkmd_darwin_dev *dev,
                         struct vk_object_base *log_obj,
                         struct nvkmd_darwin_mem *mem,
                         enum nvkmd_mem_flags flags, uint64_t size_B,
                         uint64_t align_B, uint8_t pte_kind,
                         struct nvkmd_mem **mem_out)
{
   align_B = MAX2(align_B, dev->base.pdev->bind_align_B);
   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_darwin_mem_ops,
                  flags, size_B, align_B);

   VkResult result = nvkmd_dev_alloc_va(&dev->base, log_obj, 0, pte_kind,
                                        size_B, align_B, 0, &mem->base.va);
   if (result != VK_SUCCESS)
      return result;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0, &mem->base, 0, size_B);
   if (result != VK_SUCCESS) {
      nvkmd_va_free(mem->base.va);
      return result;
   }

   *mem_out = &mem->base;
   return VK_SUCCESS;
}

static void
darwin_bo_free(struct nvkmd_darwin_dev *dev, uint64_t handle)
{
   struct TegraGM20BAbiBoFree req = { .handle = handle };
   darwin_call(&dev->conn, TegraGM20BMethodBoFree, &req, sizeof(req), NULL, 0);
}

static VkResult
nvkmd_darwin_alloc_tiled_mem(struct nvkmd_dev *_dev,
                             struct vk_object_base *log_obj,
                             uint64_t size_B, uint64_t align_B,
                             uint8_t pte_kind, uint16_t tile_mode,
                             enum nvkmd_mem_flags flags,
                             struct nvkmd_mem **mem_out)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);

   /* Unified memory: every placement is system RAM. tile_mode only matters
    * to nouveau's kernel (scanout); the GMMU only needs the kind.
    */
   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, dev->base.pdev->bind_align_B);
   size_B = align64(size_B, align_B);

   if (dev->base.pdev->debug_flags & NVK_DEBUG_FORCE_COHERENT)
      flags |= NVKMD_MEM_COHERENT;

   struct TegraGM20BAbiBoAlloc req = {
      .bytes = size_B,
      .align_log2 = CLAMP(util_logbase2_64(align_B), 12, 21),
      .flags = (flags & NVKMD_MEM_COHERENT) ? TegraGM20BBoCoherent : 0,
   };
   struct TegraGM20BAbiBoAllocReply reply = { 0 };
   kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodBoAlloc,
                                  &req, sizeof(req), &reply, sizeof(reply));
   if (kr != KERN_SUCCESS)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "BoAlloc of %" PRIu64 " B failed: 0x%x", size_B, kr);

   struct nvkmd_darwin_mem *mem = CALLOC_STRUCT(nvkmd_darwin_mem);
   if (mem == NULL) {
      darwin_bo_free(dev, reply.handle);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   mem->handle = reply.handle;
   mem->map_token = reply.map_token;

   VkResult result = darwin_mem_finish_create(dev, log_obj, mem, flags, size_B,
                                              align_B, pte_kind, mem_out);
   if (result != VK_SUCCESS) {
      darwin_bo_free(dev, reply.handle);
      FREE(mem);
   }
   return result;
}

static VkResult
nvkmd_darwin_alloc_mem(struct nvkmd_dev *dev, struct vk_object_base *log_obj,
                       uint64_t size_B, uint64_t align_B,
                       enum nvkmd_mem_flags flags, struct nvkmd_mem **mem_out)
{
   return nvkmd_darwin_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                       0 /* pte_kind */, 0 /* tile_mode */,
                                       flags, mem_out);
}

static VkResult
nvkmd_darwin_import_dma_buf(struct nvkmd_dev *dev,
                            struct vk_object_base *log_obj,
                            int fd, struct nvkmd_mem **mem_out)
{
   return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

static VkResult
nvkmd_darwin_import_host_mem(struct nvkmd_dev *_dev,
                             struct vk_object_base *log_obj,
                             void *host_ptr, uint64_t size_B,
                             struct nvkmd_mem **mem_out)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);

   struct TegraGM20BAbiBoImport req = {
      .address = (uintptr_t)host_ptr,
      .bytes = size_B,
   };
   struct TegraGM20BAbiBoAllocReply reply = { 0 };
   kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodBoImport,
                                  &req, sizeof(req), &reply, sizeof(reply));
   if (kr != KERN_SUCCESS)
      return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "BoImport failed: 0x%x", kr);

   struct nvkmd_darwin_mem *mem = CALLOC_STRUCT(nvkmd_darwin_mem);
   if (mem == NULL) {
      darwin_bo_free(dev, reply.handle);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   mem->handle = reply.handle;
   mem->host_ptr = host_ptr;

   /* The owner's mapping is cacheable: not coherent with the GPU. */
   const enum nvkmd_mem_flags flags =
      NVKMD_MEM_LOCAL | NVKMD_MEM_CAN_MAP | NVKMD_MEM_SHARED;
   VkResult result = darwin_mem_finish_create(dev, log_obj, mem, flags, size_B,
                                              0, 0, mem_out);
   if (result != VK_SUCCESS) {
      darwin_bo_free(dev, reply.handle);
      FREE(mem);
   }
   return result;
}

static void
nvkmd_darwin_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_mem->dev);

   nvkmd_va_free(mem->base.va);
   /* The kernel keeps the pages until in-flight work is done (CONTRACT.md §4). */
   darwin_bo_free(dev, mem->handle);
   FREE(mem);
}

static VkResult
nvkmd_darwin_mem_map(struct nvkmd_mem *_mem, struct vk_object_base *log_obj,
                     enum nvkmd_mem_map_flags map_flags, void *fixed_addr,
                     void **map_out)
{
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_mem->dev);

   if (mem->host_ptr != NULL) {
      if (map_flags & NVKMD_MEM_MAP_FIXED)
         return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);
      *map_out = mem->host_ptr;
      return VK_SUCCESS;
   }

   mach_vm_address_t addr = 0;
   mach_vm_size_t size = 0;
   const mach_vm_address_t fixed = (map_flags & NVKMD_MEM_MAP_FIXED) ?
                                   (uintptr_t)fixed_addr : 0;

   kern_return_t kr = darwin_map(&dev->conn, mem->map_token,
                                 !(map_flags & NVKMD_MEM_MAP_WR), fixed,
                                 &addr, &size);
   if (kr != KERN_SUCCESS || size < mem->base.size_B)
      return vk_errorf(log_obj, VK_ERROR_MEMORY_MAP_FAILED,
                       "TegraGM20B map failed: 0x%x", kr);

   *map_out = (void *)(uintptr_t)addr;
   return VK_SUCCESS;
}

static void
nvkmd_darwin_mem_unmap(struct nvkmd_mem *_mem, enum nvkmd_mem_map_flags flags,
                       void *map)
{
   struct nvkmd_darwin_mem *mem = nvkmd_darwin_mem(_mem);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_mem->dev);

   if (mem->host_ptr != NULL)
      return;

   darwin_unmap(&dev->conn, mem->map_token, (uintptr_t)map);
}

static VkResult
nvkmd_darwin_mem_overmap(struct nvkmd_mem *_mem, struct vk_object_base *log_obj,
                         enum nvkmd_mem_map_flags flags, void *map)
{
   nvkmd_darwin_mem_unmap(_mem, flags, map);

   void *new_map = mmap(map, _mem->size_B, PROT_NONE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
   if (new_map == MAP_FAILED)
      return vk_errorf(log_obj, VK_ERROR_MEMORY_MAP_FAILED,
                       "Failed to map over original mapping");
   return VK_SUCCESS;
}

static void
nvkmd_darwin_mem_sync_to_gpu(struct nvkmd_mem *mem, uint64_t offset_B,
                             uint64_t range_B)
{
   void *map = mem->client_map ? mem->client_map : mem->map;
   if (map != NULL)
      sys_dcache_flush((char *)map + offset_B, range_B);
}

static void
nvkmd_darwin_mem_sync_from_gpu(struct nvkmd_mem *mem, uint64_t offset_B,
                               uint64_t range_B)
{
   nvkmd_darwin_mem_sync_to_gpu(mem, offset_B, range_B);
}

static VkResult
nvkmd_darwin_mem_export_dma_buf(struct nvkmd_mem *mem,
                                struct vk_object_base *log_obj, int *fd_out)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static uint32_t
nvkmd_darwin_mem_log_handle(struct nvkmd_mem *mem)
{
   return (uint32_t)nvkmd_darwin_mem(mem)->handle;
}

const struct nvkmd_mem_ops nvkmd_darwin_mem_ops = {
   .free = nvkmd_darwin_mem_free,
   .map = nvkmd_darwin_mem_map,
   .unmap = nvkmd_darwin_mem_unmap,
   .overmap = nvkmd_darwin_mem_overmap,
   .sync_to_gpu = nvkmd_darwin_mem_sync_to_gpu,
   .sync_from_gpu = nvkmd_darwin_mem_sync_from_gpu,
   .export_dma_buf = nvkmd_darwin_mem_export_dma_buf,
   .log_handle = nvkmd_darwin_mem_log_handle,
};

/*
 * Virtual address ranges
 */

static VkResult
darwin_vm_bind(struct nvkmd_darwin_dev *dev, struct vk_object_base *log_obj,
               const struct TegraGM20BAbiBindOp *ops, uint32_t count)
{
   struct TegraGM20BAbiVmBind req;
   assert(count > 0 && count <= TegraGM20BMaxBindOps);
   req.count = count;
   req.reserved = 0;
   memcpy(req.ops, ops, count * sizeof(ops[0]));
   kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodVmBind, &req,
                                  offsetof(struct TegraGM20BAbiVmBind, ops) +
                                  count * sizeof(ops[0]), NULL, 0);
   return darwin_result(log_obj, kr, "VmBind");
}

static VkResult
darwin_heap_alloc(struct nvkmd_darwin_dev *dev, struct vk_object_base *log_obj,
                  enum nvkmd_va_flags flags, uint64_t size_B, uint64_t align_B,
                  uint64_t fixed_addr, uint64_t *addr_out)
{
   VkResult result = VK_SUCCESS;

   simple_mtx_lock(&dev->heap_mutex);
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      assert(flags & NVKMD_VA_REPLAY);
      if (fixed_addr < dev->replay_start || (fixed_addr & (align_B - 1)) ||
          !util_vma_heap_alloc_addr(&dev->replay_heap, fixed_addr, size_B)) {
         result = vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                            "Capture address 0x%" PRIx64 " unavailable",
                            fixed_addr);
      } else {
         *addr_out = fixed_addr;
      }
   } else {
      struct util_vma_heap *heap = (flags & NVKMD_VA_REPLAY) ?
                                   &dev->replay_heap : &dev->heap;
      *addr_out = util_vma_heap_alloc(heap, size_B, align_B);
      if (*addr_out == 0)
         result = vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "Failed to allocate virtual address range");
   }
   simple_mtx_unlock(&dev->heap_mutex);

   return result;
}

static void
darwin_heap_free(struct nvkmd_darwin_dev *dev, enum nvkmd_va_flags flags,
                 uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   util_vma_heap_free((flags & NVKMD_VA_REPLAY) ? &dev->replay_heap : &dev->heap,
                      addr, size_B);
   simple_mtx_unlock(&dev->heap_mutex);
}

static VkResult
nvkmd_darwin_alloc_va(struct nvkmd_dev *_dev, struct vk_object_base *log_obj,
                      enum nvkmd_va_flags flags, uint8_t pte_kind,
                      uint64_t size_B, uint64_t align_B, uint64_t fixed_addr,
                      struct nvkmd_va **va_out)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, _dev->pdev->bind_align_B);
   size_B = align64(size_B, align_B);
   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   struct nvkmd_darwin_va *va = CALLOC_STRUCT(nvkmd_darwin_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = darwin_heap_alloc(dev, log_obj, flags, size_B, align_B,
                                       fixed_addr, &va->base.addr);
   if (result != VK_SUCCESS) {
      FREE(va);
      return result;
   }

   if (flags & NVKMD_VA_SPARSE) {
      const struct TegraGM20BAbiBindOp op = {
         .op = TegraGM20BBindMapSparse,
         .va = va->base.addr,
         .range = size_B,
      };
      result = darwin_vm_bind(dev, log_obj, &op, 1);
      if (result != VK_SUCCESS) {
         darwin_heap_free(dev, flags, va->base.addr, size_B);
         FREE(va);
         return result;
      }
   }

   va->base.ops = &nvkmd_darwin_va_ops;
   va->base.dev = &dev->base;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;

   *va_out = &va->base;
   return VK_SUCCESS;
}

static void
nvkmd_darwin_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_va->dev);

   const struct TegraGM20BAbiBindOp op = {
      .op = TegraGM20BBindUnmap,
      .va = _va->addr,
      .range = _va->size_B,
   };
   /* If unbinding fails, leak the VA range rather than reuse it. */
   if (darwin_vm_bind(dev, NULL, &op, 1) == VK_SUCCESS)
      darwin_heap_free(dev, _va->flags, _va->addr, _va->size_B);

   FREE(nvkmd_darwin_va(_va));
}

static VkResult
nvkmd_darwin_va_bind_mem(struct nvkmd_va *va, struct vk_object_base *log_obj,
                         uint64_t va_offset_B, struct nvkmd_mem *_mem,
                         uint64_t mem_offset_B, uint64_t range_B)
{
   assert(_mem->dev == va->dev);
   const struct TegraGM20BAbiBindOp op = {
      .op = TegraGM20BBindMap,
      .kind = va->pte_kind,
      .handle = nvkmd_darwin_mem(_mem)->handle,
      .bo_offset = mem_offset_B,
      .va = va->addr + va_offset_B,
      .range = range_B,
   };
   return darwin_vm_bind(nvkmd_darwin_dev(va->dev), log_obj, &op, 1);
}

static VkResult
nvkmd_darwin_va_unbind(struct nvkmd_va *va, struct vk_object_base *log_obj,
                       uint64_t va_offset_B, uint64_t range_B)
{
   const struct TegraGM20BAbiBindOp op = {
      .op = (va->flags & NVKMD_VA_SPARSE) ? TegraGM20BBindMapSparse :
                                            TegraGM20BBindUnmap,
      .va = va->addr + va_offset_B,
      .range = range_B,
   };
   return darwin_vm_bind(nvkmd_darwin_dev(va->dev), log_obj, &op, 1);
}

const struct nvkmd_va_ops nvkmd_darwin_va_ops = {
   .free = nvkmd_darwin_va_free,
   .bind_mem = nvkmd_darwin_va_bind_mem,
   .unbind = nvkmd_darwin_va_unbind,
};

/*
 * Contexts
 */

/* ioswitch: a submission's CPU wait for a sync no one submits ends: past
 * NVKMD_DARWIN_WAIT_LIMIT_NS, one line naming the sync and then abort(), a crash
 * report with the stack instead of a silent freeze (r9-hang: r7n's and r7o's
 * freezes were a userland wait with no end; the kernel's own GPU waits panic
 * within 10 s, so a fence wait never comes near this).
 */
#define NVKMD_DARWIN_WAIT_LIMIT_NS (20ull * 1000000000ull)

static void
darwin_wait_too_long(const char *what, const struct nvkmd_darwin_sync *s,
                     uint64_t value)
{
   os_log_fault(OS_LOG_DEFAULT, "nvk darwin: %{public}s %p (value %llu) not "
                "reached after 20 s: abort() for the stack", what, (void *)s,
                (unsigned long long)value);
   abort();
}

/* GPU waits become CPU waits before the next submission (the kernel has no
 * cross-channel waits). Work already queued on this same channel needs no
 * wait: a channel executes in order.
 */
static VkResult
darwin_ctx_wait(uint32_t channel, struct vk_object_base *log_obj,
                uint32_t wait_count, const struct vk_sync_wait *waits)
{
   const uint64_t deadline = os_time_get_nano() + NVKMD_DARWIN_WAIT_LIMIT_NS;
   for (uint32_t i = 0; i < wait_count; i++) {
      struct nvkmd_darwin_sync *s = to_darwin_sync(waits[i].sync);
      if (darwin_is_timeline(s)) {
         VkResult result = darwin_timeline_wait(s, waits[i].wait_value, 0,
                                                channel, deadline);
         if (result == VK_TIMEOUT)
            darwin_wait_too_long("timeline wait before a submission", s,
                                 waits[i].wait_value);
         if (result != VK_SUCCESS)
            return vk_error(log_obj, VK_ERROR_DEVICE_LOST);
         continue;
      }
      VkResult result = darwin_sync_wait_resolved(s, deadline);
      if (result == VK_TIMEOUT)
         darwin_wait_too_long("wait for a sync to be submitted", s, 0);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, VK_ERROR_DEVICE_LOST);

      pthread_mutex_lock(&darwin_sync_lock);
      const bool same_channel = s->state == NVKMD_DARWIN_SYNC_FENCE &&
                                channel != 0 && s->channel == channel;
      pthread_mutex_unlock(&darwin_sync_lock);
      if (same_channel)
         continue;

      result = darwin_sync_wait_one(s, 0, deadline);
      if (result == VK_TIMEOUT)
         darwin_wait_too_long("binary wait before a submission", s, 0);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, VK_ERROR_DEVICE_LOST);
   }
   return VK_SUCCESS;
}

static VkResult
darwin_create_exec_ctx(struct nvkmd_darwin_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_engines engines, struct nvkmd_ctx **ctx_out)
{
   STATIC_ASSERT(NVKMD_ENGINE_COPY == (int)TegraGM20BEngineCopy);
   STATIC_ASSERT(NVKMD_ENGINE_2D == (int)TegraGM20BEngine2D);
   STATIC_ASSERT(NVKMD_ENGINE_3D == (int)TegraGM20BEngine3D);
   STATIC_ASSERT(NVKMD_ENGINE_M2MF == (int)TegraGM20BEngineM2MF);
   STATIC_ASSERT(NVKMD_ENGINE_COMPUTE == (int)TegraGM20BEngineCompute);

   /* Reserve a channel slot (UINT32_MAX until the kernel names the channel). */
   simple_mtx_lock(&dev->channels_mutex);
   struct nvkmd_darwin_channel *slot = darwin_channel_locked(dev, 0);
   if (slot != NULL)
      slot->id = UINT32_MAX;
   simple_mtx_unlock(&dev->channels_mutex);
   if (slot == NULL)
      return vk_error(log_obj, VK_ERROR_TOO_MANY_OBJECTS);

   struct nvkmd_darwin_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_darwin_exec_ctx);
   if (ctx == NULL) {
      slot->id = 0;
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   const struct TegraGM20BAbiChannelCreate req = { .engines = engines };
   struct TegraGM20BAbiChannelCreateReply reply = { 0 };
   kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodChannelCreate,
                                  &req, sizeof(req), &reply, sizeof(reply));
   if (kr != KERN_SUCCESS) {
      slot->id = 0;
      FREE(ctx);
      return kr == kIOReturnNoResources ?
             vk_error(log_obj, VK_ERROR_TOO_MANY_OBJECTS) :
             darwin_result(log_obj, kr, "ChannelCreate");
   }

   mach_vm_address_t addr = 0;
   mach_vm_size_t size = 0;
   kr = darwin_map(&dev->conn, reply.fence_token, true, 0, &addr, &size);
   if (kr != KERN_SUCCESS || reply.fence_offset + 4 > size) {
      const struct TegraGM20BAbiChannel destroy = { .channel = reply.channel };
      darwin_call(&dev->conn, TegraGM20BMethodChannelDestroy, &destroy,
                  sizeof(destroy), NULL, 0);
      slot->id = 0;
      FREE(ctx);
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "fence page map failed: 0x%x", kr);
   }

   simple_mtx_lock(&dev->channels_mutex);
   *slot = (struct nvkmd_darwin_channel) {
      .id = reply.channel,
      .fence = (const volatile uint32_t *)(uintptr_t)(addr + reply.fence_offset),
      .fence_map = addr,
      .fence_token = reply.fence_token,
   };
   simple_mtx_unlock(&dev->channels_mutex);

   ctx->base.ops = &nvkmd_darwin_exec_ctx_ops;
   ctx->base.dev = &dev->base;
   ctx->channel = reply.channel;
   ctx->req.channel = reply.channel;

   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

static void
nvkmd_darwin_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_ctx->dev);

   /* The kernel waits for idle before tearing the channel down. */
   const struct TegraGM20BAbiChannel req = { .channel = ctx->channel };
   darwin_call(&dev->conn, TegraGM20BMethodChannelDestroy, &req, sizeof(req),
               NULL, 0);

   simple_mtx_lock(&dev->channels_mutex);
   struct nvkmd_darwin_channel *chan = darwin_channel_locked(dev, ctx->channel);
   if (chan != NULL) {
      darwin_unmap(&dev->conn, chan->fence_token, chan->fence_map);
      memset(chan, 0, sizeof(*chan));
   }
   simple_mtx_unlock(&dev->channels_mutex);

   FREE(ctx);
}

static VkResult
nvkmd_darwin_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t wait_count,
                           const struct vk_sync_wait *waits)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);
   return darwin_ctx_wait(ctx->channel, log_obj, wait_count, waits);
}

static VkResult
nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_ctx->dev);

   if (ctx->req.count == 0)
      return VK_SUCCESS;

   const size_t in_size = offsetof(struct TegraGM20BAbiExec, pushes) +
                          ctx->req.count * sizeof(ctx->req.pushes[0]);
   for (;;) {
      struct TegraGM20BAbiExecReply reply = { 0 };
      kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodExec,
                                     &ctx->req, in_size, &reply, sizeof(reply));
      if (kr == KERN_SUCCESS) {
         ctx->last_seqno = reply.seqno;
         break;
      }
      if (kr != kIOReturnBusy)
         return darwin_result(log_obj, kr, "Exec");

      /* GPFIFO full: let the work already queued drain. */
      if (ctx->last_seqno == 0) {
         usleep(100);
         continue;
      }
      VkResult result = darwin_wait_seqno(dev, ctx->channel, ctx->last_seqno,
                                          NVKMD_DARWIN_FOREVER_NS);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, VK_ERROR_DEVICE_LOST);
   }

   ctx->req.count = 0;
   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t exec_count,
                           const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count; i++) {
      /* A push that ends mid-method must share a submission with the next. */
      uint32_t chained = 0;
      for (uint32_t j = i; j < exec_count && execs[j].incomplete; j++)
         chained++;
      assert(chained < TegraGM20BMaxPushes);

      if (ctx->req.count + chained >= TegraGM20BMaxPushes) {
         VkResult result = nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      assert((execs[i].addr % 4) == 0 && (execs[i].size_B % 4) == 0);
      assert(execs[i].size_B < (1u << 23));
      ctx->req.pushes[ctx->req.count++] = (struct TegraGM20BAbiPush) {
         .va = execs[i].addr,
         .dwords = execs[i].size_B / 4,
         .flags = execs[i].no_prefetch ? TegraGM20BPushNoPrefetch : 0,
      };
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t signal_count,
                             const struct vk_sync_signal *signals)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_ctx->dev);

   VkResult result = nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   /* Everything this channel was given before now is behind last_seqno. */
   for (uint32_t i = 0; i < signal_count; i++) {
      struct nvkmd_darwin_sync *s = to_darwin_sync(signals[i].sync);
      if (darwin_is_timeline(s))
         darwin_timeline_signal(s, signals[i].signal_value,
                                ctx->last_seqno ? dev : NULL, ctx->channel,
                                ctx->last_seqno);
      else if (ctx->last_seqno == 0)
         darwin_sync_set(s, NVKMD_DARWIN_SYNC_SIGNALED, NULL, 0, 0);
      else
         darwin_sync_set(s, NVKMD_DARWIN_SYNC_FENCE, dev, ctx->channel,
                         ctx->last_seqno);
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   struct nvkmd_darwin_exec_ctx *ctx = nvkmd_darwin_exec_ctx(_ctx);
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_ctx->dev);

   VkResult result = nvkmd_darwin_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   if (ctx->last_seqno != 0) {
      result = darwin_wait_seqno(dev, ctx->channel, ctx->last_seqno,
                                 NVKMD_DARWIN_FOREVER_NS);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, VK_ERROR_DEVICE_LOST);
   }

   /* Check for channel errors, as nvkmd_nouveau's empty exec does. */
   const struct TegraGM20BAbiChannel req = { .channel = ctx->channel };
   struct TegraGM20BAbiStatus status = { 0 };
   kern_return_t kr = darwin_call(&dev->conn, TegraGM20BMethodGetStatus,
                                  &req, sizeof(req), &status, sizeof(status));
   if (kr != KERN_SUCCESS || status.error != 0)
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "channel %u error %u at 0x%" PRIx64, ctx->channel,
                       status.error, status.fault_va);

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_darwin_exec_ctx_ops = {
   .destroy = nvkmd_darwin_exec_ctx_destroy,
   .wait = nvkmd_darwin_exec_ctx_wait,
   .exec = nvkmd_darwin_exec_ctx_exec,
   .signal = nvkmd_darwin_exec_ctx_signal,
   .flush = nvkmd_darwin_exec_ctx_flush,
   .sync = nvkmd_darwin_exec_ctx_sync,
};

static void
nvkmd_darwin_bind_ctx_destroy(struct nvkmd_ctx *ctx)
{
   FREE(nvkmd_darwin_bind_ctx(ctx));
}

static VkResult
nvkmd_darwin_bind_ctx_wait(struct nvkmd_ctx *ctx,
                           struct vk_object_base *log_obj,
                           uint32_t wait_count,
                           const struct vk_sync_wait *waits)
{
   /* Binds run synchronously on the CPU: everything they wait on must be done. */
   return darwin_ctx_wait(0, log_obj, wait_count, waits);
}

static VkResult
nvkmd_darwin_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj)
{
   struct nvkmd_darwin_bind_ctx *ctx = nvkmd_darwin_bind_ctx(_ctx);

   if (ctx->req.count == 0)
      return VK_SUCCESS;

   VkResult result = darwin_vm_bind(nvkmd_darwin_dev(_ctx->dev), log_obj,
                                    ctx->req.ops, ctx->req.count);
   ctx->req.count = 0;
   ctx->prev_va = NULL;
   return result;
}

static VkResult
nvkmd_darwin_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj,
                           uint32_t bind_count,
                           const struct nvkmd_ctx_bind *binds)
{
   struct nvkmd_darwin_bind_ctx *ctx = nvkmd_darwin_bind_ctx(_ctx);

   for (uint32_t i = 0; i < bind_count; i++) {
      struct TegraGM20BAbiBindOp op = {
         .kind = binds[i].va->pte_kind,
         .va = binds[i].va->addr + binds[i].va_offset_B,
         .range = binds[i].range_B,
      };
      if (binds[i].op == NVKMD_BIND_OP_BIND) {
         op.op = TegraGM20BBindMap;
         op.handle = nvkmd_darwin_mem(binds[i].mem)->handle;
         op.bo_offset = binds[i].mem_offset_B;
      } else {
         op.op = (binds[i].va->flags & NVKMD_VA_SPARSE) ?
                 TegraGM20BBindMapSparse : TegraGM20BBindUnmap;
      }

      /* Coalesce contiguous ops of one VA, as nvkmd_nouveau does. */
      if (ctx->req.count > 0 && binds[i].va == ctx->prev_va) {
         struct TegraGM20BAbiBindOp *prev = &ctx->req.ops[ctx->req.count - 1];
         if (op.op == prev->op && op.kind == prev->kind &&
             op.handle == prev->handle &&
             op.va == prev->va + prev->range &&
             (op.op != TegraGM20BBindMap ||
              op.bo_offset == prev->bo_offset + prev->range)) {
            prev->range += op.range;
            continue;
         }
      }

      if (ctx->req.count >= TegraGM20BMaxBindOps) {
         VkResult result = nvkmd_darwin_bind_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      ctx->req.ops[ctx->req.count++] = op;
      ctx->prev_va = binds[i].va;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t signal_count,
                             const struct vk_sync_signal *signals)
{
   VkResult result = nvkmd_darwin_bind_ctx_flush(_ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < signal_count; i++) {
      struct nvkmd_darwin_sync *s = to_darwin_sync(signals[i].sync);
      if (darwin_is_timeline(s))
         darwin_timeline_signal(s, signals[i].signal_value, NULL, 0, 0);
      else
         darwin_sync_set(s, NVKMD_DARWIN_SYNC_SIGNALED, NULL, 0, 0);
   }
   return VK_SUCCESS;
}

static VkResult
nvkmd_darwin_bind_ctx_sync(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   return nvkmd_darwin_bind_ctx_flush(_ctx, log_obj);
}

const struct nvkmd_ctx_ops nvkmd_darwin_bind_ctx_ops = {
   .destroy = nvkmd_darwin_bind_ctx_destroy,
   .wait = nvkmd_darwin_bind_ctx_wait,
   .bind = nvkmd_darwin_bind_ctx_bind,
   .signal = nvkmd_darwin_bind_ctx_signal,
   .flush = nvkmd_darwin_bind_ctx_flush,
   .sync = nvkmd_darwin_bind_ctx_sync,
};

static VkResult
nvkmd_darwin_create_ctx(struct nvkmd_dev *_dev, struct vk_object_base *log_obj,
                        enum nvkmd_engines engines, struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_darwin_dev *dev = nvkmd_darwin_dev(_dev);

   if (engines == NVKMD_ENGINE_BIND) {
      struct nvkmd_darwin_bind_ctx *ctx = CALLOC_STRUCT(nvkmd_darwin_bind_ctx);
      if (ctx == NULL)
         return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      ctx->base.ops = &nvkmd_darwin_bind_ctx_ops;
      ctx->base.dev = &dev->base;
      *ctx_out = &ctx->base;
      return VK_SUCCESS;
   }

   assert(!(engines & NVKMD_ENGINE_BIND));
   return darwin_create_exec_ctx(dev, log_obj, engines, ctx_out);
}

/*
 * Loader entry point
 */

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* iPadOS has no Khronos loader: the Metal frontend opens this library by path
 * and looks up the loader's name, so the driver answers it itself (as
 * UniXNU's Turnip darwin port does).
 */
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return vk_icdGetInstanceProcAddr(instance, pName);
}
