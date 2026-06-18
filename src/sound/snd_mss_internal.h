#include "snd_local.h"

struct MssFileHandle // sizeof=0x9C
{                                       // ...
    uint32_t id;
    MssFileHandle *next;
    int handle;
    char fileName[128];
    uint32_t hashCode;
    int offset;
    int fileOffset;
    int fileLength;
};

struct MssEqInfo // sizeof=0xF00
{                                       // ...
    SndEqParams params[3][64];
};

typedef struct _SAMPLE FAR *HSAMPLE;           // Handle to sample

struct MssLocal // sizeof=0x26D0
{                                       // ...
    _DIG_DRIVER *driver;                // ...
    HSAMPLE handle_sample[40];         // ...
    _STREAM *handle_stream[13];
    MssEqInfo eq[2];                    // ...
    uint32_t eqFilter;              // ...
    MssFileHandle fileHandle[13];
    MssFileHandle *freeFileHandle;
    bool isMultiChannel;                // ...
    // padding byte
    // padding byte
    // padding byte
};

extern MssLocal milesGlob;