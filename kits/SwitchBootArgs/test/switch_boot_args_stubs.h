// PE_parse_boot_argn for kits/SwitchBootArgs on the host, over a table the test fills.
#ifndef SWITCH_TEST_SWITCH_BOOT_ARGS_STUBS_H
#define SWITCH_TEST_SWITCH_BOOT_ARGS_STUBS_H

#ifdef __cplusplus
extern "C" {
#endif

int PE_parse_boot_argn(const char *name, void *value, int size);

#ifdef __cplusplus
}
#endif

#endif
