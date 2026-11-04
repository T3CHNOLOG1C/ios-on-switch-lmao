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
bool nvdec_claim_self_test(NVDECClient *client);  // true for one caller per boot (TegraNVDECMethodClaimSelfTest)

// The H264Backend buffer and job callbacks (ctx = the client).
int nvdec_alloc(void *client, uint64_t size, uint32_t *handle, void **va);
void nvdec_free(void *client, uint32_t handle);
int nvdec_submit(void *client, const TegraNVDECSubmit *submit, uint32_t *fence);
int nvdec_wait(void *client, const TegraNVDECWait *wait);

#endif
