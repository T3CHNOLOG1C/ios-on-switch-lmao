// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TegraNVDEC user client from user space (ABI: drivers/TegraNVDEC/TegraNVDECABI.h). Calls are tunnelled through
// the first selector the process's sandbox admits; buffers are mapped cacheable into this task.
#ifndef NVDEC_CLIENT_H
#define NVDEC_CLIENT_H
#include <stdbool.h>
#include <stdint.h>
#include "../../TegraNVDEC/TegraNVDECABI.h"

typedef struct NVDECClient NVDECClient;

bool nvdec_service_present(void);
NVDECClient *nvdec_open(int *error);  // NULL with the IOReturn when the service is absent or refuses this process
void nvdec_close(NVDECClient *client);
// TegraNVDECInfo.codecs: bit n, codec n proven on the engine at boot, as the kernel had them when the client opened.
uint32_t nvdec_codecs(const NVDECClient *client);
// The same bits read again (a clip-check verdict since the open turns a failed codec's bit off); the open's on error.
uint32_t nvdec_codecs_now(NVDECClient *client);
// The clip check's verdict (TegraNVDECABI.h TEGRA_NVDEC_CHECK_*): claim (the state; *claimed when this connection runs
// it), report (the owner: VP9, HEVC and HEVC Main 10 verdicts), wait (at most 3 s while another process runs it).
uint32_t nvdec_check_claim(NVDECClient *client, bool *claimed);
int nvdec_check_report(NVDECClient *client, bool vp9Ok, bool hevcOk, bool hevc10Ok);
uint32_t nvdec_check_wait(NVDECClient *client);

// The H264Backend buffer and job callbacks (ctx = the client).
int nvdec_alloc(void *client, uint64_t size, uint32_t *handle, void **va);
void nvdec_free(void *client, uint32_t handle);
int nvdec_submit(void *client, const TegraNVDECSubmit *submit, uint32_t *fence);
int nvdec_wait(void *client, const TegraNVDECWait *wait);

#endif
