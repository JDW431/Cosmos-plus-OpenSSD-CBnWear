#include <stdio.h>
#include "address_translation.h"
#include "metadata_export.h"

void MetadataReceiveByte(MetadataExport *scan, int ch, MetadataWriteLine writeLine)
{
    static const char command[] = "export";
    char line[64];
    int matched;
    if (ch != '\r' && ch != '\n') {
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
        if (scan->commandLength >= 0 && scan->commandLength < (int)sizeof(command) - 1 &&
            ch == command[scan->commandLength])
            scan->commandLength++;
        else
            scan->commandLength = -1;  /* Ignore this line until Enter. */
        return;
    }
    matched = scan->commandLength == (int)sizeof(command) - 1;
    scan->commandLength = 0;
    if (!matched || scan->active)
        return;
    scan->active = 1;
    scan->next = 0;
    snprintf(line, sizeof(line), "META_BEGIN,1,%u,%u\r\n",
             (unsigned int)USER_DIES, (unsigned int)USER_BLOCKS_PER_DIE);
    writeLine(line);
}

void MetadataStep(MetadataExport *scan, MetadataReadAge readAge, MetadataWriteLine writeLine)
{
    unsigned int die, block, age;
    char line[112], ageText[16] = "NA";
    const VIRTUAL_BLOCK_ENTRY *entry;
    if (!scan->active)
        return;
    if (scan->next == USER_DIES * USER_BLOCKS_PER_DIE) {
        snprintf(line, sizeof(line), "META_END,%u\r\n", scan->next);
        writeLine(line);
        scan->active = 0;
        return;
    }
    die = scan->next / USER_BLOCKS_PER_DIE;
    block = scan->next % USER_BLOCKS_PER_DIE;
    entry = &virtualBlockMapPtr->block[die][block];
    if (readAge && readAge(die, block, &age))
        snprintf(ageText, sizeof(ageText), "%u", age);
    snprintf(line, sizeof(line), "META_BLOCK,%u,%u,%u,%s,%u,%u\r\n",
             die, block, (unsigned int)entry->eraseCnt, ageText,
             (unsigned int)entry->bad, (unsigned int)entry->free);
    writeLine(line);
    scan->next++;
}
