#include "Debug.h"
#include <stdio.h>
#include <string.h>
#include "OutputConsole.h"
void debug_tls(const char* str)
{
    // dprintf() already appends every line to LOG_FILE_PATH ("game:\
    // DebugInfo.txt") itself - this used to ALSO independently open, write,
    // and close that exact same file right after, writing the same line
    // twice with two separate open/close cycles for no reason.
    if(str)
        dprintf("%s\n", str);
}
void debug_hex(const char* label, const unsigned char* buf, int len)
{
    char line[128];
    char ascii[17];
    int i;

    debug_tls(label);
    for (i = 0; i < len; i++) {
        if (i % 16 == 0) {
            if (i != 0) {
                ascii[16] = 0;
                sprintf(line + strlen(line), "  | %s", ascii);
                debug_tls(line);
            }
            sprintf(line, "%08X: ", i);
        }

        sprintf(line + strlen(line), "%02X ", buf[i]);

        ascii[i % 16] = (buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.';
    }

    // Final line
    if (i % 16 != 0) {
        int pad = (16 - (i % 16)) * 3;
        for (int j = 0; j < pad; j++) {
            strcat(line, " ");
        }
        ascii[i % 16] = 0;
        sprintf(line + strlen(line), "  | %s", ascii);
        debug_tls(line);
    }
}

