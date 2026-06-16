#include "snd_local.h"
#include "snd_public.h"
#include <universal/com_files.h>
#include <universal/com_memory.h>


LoadedSound* __cdecl SND_LoadFromBuffer(void* buffer, const char* soundName)
{
    if (!buffer) return 0;

#ifdef USE_OPENAL
    uint8_t* buf = (uint8_t*)buffer;
    size_t memorySize = 0;
    
    // Extract exact buffer size from the RIFF header
    if (memcmp(buf, "RIFF", 4) == 0) {
        memorySize = *(uint32_t*)(buf + 4) + 8;
    } else {
        Com_PrintError(1, "OPENAL WARNING: Non-RIFF file in memory load: %s\n", soundName);
        return 0;
    }

    drwav wavDecoder;
    
    if (drwav_init_memory(&wavDecoder, buffer, memorySize, NULL)) {
        uint64_t totalFrames = wavDecoder.totalPCMFrameCount;
        uint16_t channels = wavDecoder.channels;
        ALsizei rate = wavDecoder.sampleRate;
        ALenum format = (channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        
        if (totalFrames > 0) {
            size_t decodedDataLength = totalFrames * channels * sizeof(int16_t);
            
            // --- THE CRASH & SILENCE FIX ---
            // 1. Allocate space inside the engine's managed Zone Memory.
            // This prevents heap corruption and allows cleanup loops to run safely on exit.
            int16_t* pDecodedData = (int16_t*)Z_Malloc(decodedDataLength, "SND_PCM_DATA", 15);
            
            if (pDecodedData) {
                // Decode the pristine PCM frames directly into our allocated Zone buffer
                drwav_read_pcm_frames_s16(&wavDecoder, totalFrames, (drwav_int16*)pDecodedData);

                LoadedSound* loadSnd = (LoadedSound*)Hunk_Alloc(sizeof(LoadedSound), "SND_LoadFromBuffer", 15);
                memset(loadSnd, 0, sizeof(LoadedSound)); 

                loadSnd->name = soundName;
                loadSnd->sound.info.format = 1; // 1 = Standard Raw PCM
                loadSnd->sound.info.rate = rate;
                loadSnd->sound.info.data_len = decodedDataLength;
                loadSnd->sound.info.channels = channels;
                loadSnd->sound.info.bits = 16; 
                loadSnd->sound.info.samples = totalFrames;
                loadSnd->sound.info.block_size = channels * sizeof(int16_t);

                // 2. Link all pointer entries to our live data block.
                // The mixing loops and facial structures read from here—unmuting voice lines!
                loadSnd->sound.data = (uint8_t*)pDecodedData; 
                loadSnd->sound.info.data_ptr = loadSnd->sound.data;
                loadSnd->sound.info.initial_ptr = loadSnd->sound.data;

                // Pass the data out to OpenAL Hardware
                alGenBuffers(1, &loadSnd->sound.oalBuffer);
                alBufferData(loadSnd->sound.oalBuffer, format, pDecodedData, decodedDataLength, rate);

                // 3. CRITICAL: Do NOT call free() or Z_Free() here! 
                // The engine now fully owns this allocation and will clean it up on mission shutdown.
                
                drwav_uninit(&wavDecoder);
                return loadSnd;
            }
        }
        drwav_uninit(&wavDecoder);
    }
#else
    // --- Original MSS logic ---
    _AILSOUNDINFO info; // [esp+8h] [ebp-28h] BYREF
    LoadedSound *loadSnd; // [esp+2Ch] [ebp-4h]

    if (!buffer)
        MyAssertHandler(".\\win32\\snd_driver_load_obj.cpp", 134, 0, "%s", "buffer");
    if (AIL_WAV_info(buffer, &info))
    {
        if (info.data_len)
        {
            loadSnd = (LoadedSound*)Hunk_Alloc(0x2Cu, "SND_LoadFromBuffer", 15);
            loadSnd->name = soundName;
            qmemcpy(&loadSnd->sound, &info, 0x24u);
            SND_SetData(&loadSnd->sound, (void*)info.data_ptr);
            return loadSnd;
        }
        else
        {
            Com_PrintError(1, "ERROR: Sound file '%s' is zero length, invalid\n", soundName);
            return 0;
        }
    }
#endif
    else
    {
        Com_PrintError(1, "ERROR: Sound file '%s' is in an invalid or corrupted format\n", soundName);
        return 0;
    }
}

LoadedSound *__cdecl SND_LoadSoundFile(const char *name)
{
    void *buffer; // [esp+4h] [ebp-10Ch] BYREF
    char realname[256]; // [esp+8h] [ebp-108h] BYREF
    LoadedSound *loadSnd; // [esp+10Ch] [ebp-4h]

    if (IsFastFileLoad())
        MyAssertHandler(".\\win32\\snd_driver_load_obj.cpp", 175, 0, "%s", "IsObjFileLoad()");
    if (!name)
        MyAssertHandler(".\\win32\\snd_driver_load_obj.cpp", 176, 0, "%s", "name");
    Com_sprintf(realname, 0x100u, "sound/%s", name);
    if (FS_ReadFile(realname, &buffer) >= 0)
    {
        loadSnd = SND_LoadFromBuffer(buffer, name);
        FS_FreeFile((char*)buffer);
        return loadSnd;
    }
    else
    {
        Com_PrintError(1, "ERROR: Sound file '%s' not found\n", realname);
        return 0;
    }
}