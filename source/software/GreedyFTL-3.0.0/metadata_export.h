#ifndef METADATA_EXPORT_H
#define METADATA_EXPORT_H

typedef struct {
    unsigned int next;
    int active;
    int commandLength;
} MetadataExport;

typedef void (*MetadataWriteLine)(const char *text);
/* Return 1 only when this block has a defined age in the policy's units. */
typedef int (*MetadataReadAge)(unsigned int die, unsigned int block, unsigned int *age);

/* Feed available bytes after FTL initialization. "export" + CR or LF starts a scan. */
void MetadataReceiveByte(MetadataExport *scan, int ch, MetadataWriteLine writeLine);
/* Emit at most one line per call. Keep host I/O stopped during a scan. */
void MetadataStep(MetadataExport *scan, MetadataReadAge readAge, MetadataWriteLine writeLine);

#endif
