/*
 * Registration point of the libImage4_V2 interface (Darwin 26 KPI).
 *
 * The Image4 kext publishes its interface table here at start. This kernel
 * keeps the pointer only; it has no consumers of the table yet.
 */
#include <libkern/libkern.h>
#include <libkern/section_keywords.h>

void image4_v2_interface_register(const void *interface);

#pragma mark Module Globals
SECURITY_READ_ONLY_LATE(const void *) _image4_v2_interface = NULL;

#pragma mark KPI
void
image4_v2_interface_register(const void *interface)
{
	if (_image4_v2_interface) {
		panic("libImage4_V2 interface already set");
	}
	_image4_v2_interface = interface;
}
