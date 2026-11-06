/*
 * Public stand-in for the CoreEntitlements V2 interface. The kernel only
 * stores the registered implementation pointer and passes context pointers
 * through, so both types stay opaque.
 */
#ifndef CORE_ENTITLEMENTS_V2_API_H
#define CORE_ENTITLEMENTS_V2_API_H

typedef struct CEKernelAPI CEKernelAPI_t;
typedef struct CEContext CEContext_t;

#endif /* CORE_ENTITLEMENTS_V2_API_H */
