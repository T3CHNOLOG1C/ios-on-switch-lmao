/* a64class: read hex words on stdin, print a64x's class name for each (for the LLVM oracle). */
#include "../a64x/a64x.h"
#include <stdio.h>
#include <stdlib.h>

int a64x_cmd_class(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        uint32_t w = (uint32_t)strtoul(line, NULL, 16);
        puts(a64x_class_name(a64x_classify(w)));
    }
    return 0;
}
