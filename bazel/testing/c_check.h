/* Checks in C test code (a model or scenario kept in C, built with the C flags the code under test has), reported
 * through GoogleTest at the C file and line (c_check.cpp). Unlike assert() they stay under --config=release (NDEBUG).
 *
 *   CHECK(r.result == LOADER_EMC_RAISED);       a failure at this line if false; the scenario goes on
 *   if (!CHECK(p != NULL)) return 0;            stop where later code relies on the check
 *   FAIL_C("read outside the model: %#x", pa);  a failure described by the C code
 */
#ifndef IOSWITCH_TEST_C_CHECK_H
#define IOSWITCH_TEST_C_CHECK_H
#ifdef __cplusplus
extern "C" {
#endif
/* Records a GoogleTest failure at file:line when !ok. Returns ok. */
int iosw_test_check(int ok, const char *expression, const char *file, int line);
/* Records a GoogleTest failure at file:line with a printf-style message. */
void iosw_test_fail(const char *file, int line, const char *format, ...) __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif
#define CHECK(x)    iosw_test_check(!!(x), #x, __FILE__, __LINE__)
#define FAIL_C(...) iosw_test_fail(__FILE__, __LINE__, __VA_ARGS__)
#endif
