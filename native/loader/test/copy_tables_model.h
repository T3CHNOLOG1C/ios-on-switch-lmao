/* copy_tables_model.c's entry points, for copy_tables_test.cpp. */
#ifndef IOSWITCH_TEST_COPY_TABLES_MODEL_H
#define IOSWITCH_TEST_COPY_TABLES_MODEL_H
#ifdef __cplusplus
extern "C" {
#endif
int copy_tables_setup(void);
void copy_tables_teardown(void);
unsigned copy_tables_sizes(void);
unsigned copy_tables_offsets(void);
/* One exact 4 GiB map check: window size `n`, destination offset `o` (indexes into the model's tables). */
void copy_tables_positive(unsigned n, unsigned o);
/* The malformed-input controls; returns how many ran. */
unsigned copy_tables_negatives(void);
#ifdef __cplusplus
}
#endif
#endif
