// SPDX-License-Identifier: MIT
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// Logs what UIKit and Core Animation take the display's frame rate to be, 10 s after the first call (debug builds; once).
void switch_display_rate_probe(void);
#ifdef __cplusplus
}
#endif
