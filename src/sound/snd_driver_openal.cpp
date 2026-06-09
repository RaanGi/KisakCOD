#include "snd_local.h"
#include "snd_public.h"
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_cinematic.h>
#include <universal/com_sndalias.h>

#ifdef USE_OPENAL

MssLocal milesGlob;
OalLocal oalGlob;

// --- MASTER VFS CALLBACKS ---

size_t VFS_Read_Global(void* pUserData, void* pBufferOut, size_t bytesToRead) {
    int fileHandle = (int)(intptr_t)pUserData;
    
    // dr_libs passes a NULL buffer when it just wants to skip bytes.
    // We must handle this safely using the engine's SEEK_CUR (which is 0).
    if (!pBufferOut) {
        FS_Seek(fileHandle, (int)bytesToRead, 0); 
        return bytesToRead;
    }
    
    uint8_t* outBuffer = (uint8_t*)pBufferOut;
    size_t totalRead = 0;

    // Force the VFS to fulfill the entire read request unless we hit physical EOF
    while (totalRead < bytesToRead) {
        int bytesRead = FS_Read(outBuffer + totalRead, (uint32_t)(bytesToRead - totalRead), fileHandle);
        if (bytesRead <= 0) break; // Physical EOF or VFS error
        totalRead += bytesRead;
    }
    
    return totalRead;
}

drmp3_bool32 VFS_Seek_Global(void* pUserData, int offset, drmp3_seek_origin origin) {
    int fileHandle = (int)(intptr_t)pUserData;
    int iw3_origin;
    
    // Map dr_libs standard origins perfectly to Quake/IW3 Engine origins
    switch(origin) {
        case DRMP3_SEEK_SET: iw3_origin = 2; break; // Engine SEEK_SET
        case DRMP3_SEEK_CUR: iw3_origin = 0; break; // Engine SEEK_CUR
        case DRMP3_SEEK_END: iw3_origin = 1; break; // Engine SEEK_END
        default: return DRMP3_FALSE;
    }
    
    FS_Seek(fileHandle, offset, iw3_origin);
    return DRMP3_TRUE;
}

drmp3_bool32 VFS_Tell_Global(void* pUserData, drmp3_int64* pCursor) {
    int fileHandle = (int)(intptr_t)pUserData;
    if (pCursor) *pCursor = (drmp3_int64)FS_FTell(fileHandle);
    return DRMP3_TRUE;
}

const dvar_t *snd_khz;
const dvar_t *snd_outputConfiguration;

void __cdecl TRACK_snd_driver() {}
bool __cdecl SND_IsMultiChannel() { return oalGlob.isMultiChannel; }

char __cdecl SND_InitDriver()
{
    snd_khz = Dvar_RegisterInt("snd_khz", 44, (DvarLimits)0x2C0000000BLL, DVAR_ARCHIVE | DVAR_LATCH, "The game sound frequency.");
    snd_outputConfiguration = Dvar_RegisterEnum("snd_outputConfiguration", snd_outputConfigurationStrings, 0, DVAR_ARCHIVE | DVAR_LATCH, "Sound output configuration");

    int hertz = (snd_khz->current.integer == 11) ? 11025 : (snd_khz->current.integer == 44 ? 44100 : 22050);

    oalGlob.device = alcOpenDevice(nullptr);
    if (!oalGlob.device) {
        Com_PrintError(9, "AUDIO ERROR: Couldn't open OpenAL device.\n");
        return 0;
    }

    ALCint attribs[] = { ALC_FREQUENCY, hertz, 0 };
    oalGlob.context = alcCreateContext(oalGlob.device, attribs);
    if (!oalGlob.context || !alcMakeContextCurrent(oalGlob.context)) {
        Com_PrintError(9, "AUDIO ERROR: Couldn't create OpenAL context.\n");
        if (oalGlob.context) alcDestroyContext(oalGlob.context);
        alcCloseDevice(oalGlob.device);
        return 0;
    }

    alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);

    g_snd.Initialized2d = 1;
    g_snd.Initialized3d = 1;
    g_snd.max_2D_channels = 8;
    g_snd.max_3D_channels = 32;
    g_snd.max_stream_channels = 13;
    g_snd.playback_rate = hertz + (hertz / 2);
    if (g_snd.playback_rate >= 0xAC44) g_snd.playback_rate = 0x7FFFFFFF;
    g_snd.playback_channels = 2; 
    g_snd.timescale = 1.0;
    oalGlob.isMultiChannel = true;
    
    alGenSources(g_snd.max_2D_channels + g_snd.max_3D_channels, oalGlob.sources);

    // Initialize Streams
    for (int i = 0; i < g_snd.max_stream_channels; i++) {
        alGenSources(1, &oalGlob.streams[i].source);
        alGenBuffers(NUM_STREAM_BUFFERS, oalGlob.streams[i].buffers);
        oalGlob.streams[i].active = false;
        oalGlob.streams[i].fileHandle = 0;
    }
    
    g_snd.ambient_track = 1;

    Com_Printf(9, "OpenAL initialized successfully at %i Hz\n", hertz);
    return 1;
}

void __cdecl SND_ShutdownDriver()
{
    R_Cinematic_StopPlayback();
    R_Cinematic_SyncNow();

    alDeleteSources(g_snd.max_2D_channels + g_snd.max_3D_channels, oalGlob.sources);
    
    for (int i = 0; i < g_snd.max_stream_channels; i++) {
        OalStream* stream = &oalGlob.streams[i];
        
        alSourceStop(stream->source);
        alSourcei(stream->source, AL_BUFFER, 0);

        if (stream->streamType == 1) drmp3_uninit(&stream->mp3Decoder);
        else if (stream->streamType == 2) drwav_uninit(&stream->wavDecoder);
        stream->streamType = 0;

        if (stream->fileHandle) {
            FS_FCloseFile(stream->fileHandle);
            stream->fileHandle = 0;
        }

        alDeleteSources(1, &stream->source);
        alDeleteBuffers(NUM_STREAM_BUFFERS, stream->buffers);
    }

    alcMakeContextCurrent(nullptr);
    if (oalGlob.context) alcDestroyContext(oalGlob.context);
    if (oalGlob.device) alcCloseDevice(oalGlob.device);
    memset(&oalGlob, 0, sizeof(oalGlob));
}

int __cdecl SND_GetDriverCPUPercentage() { return 1; }

void __cdecl SND_Set3DPosition(int index, const float *org)
{
    if (index >= 8 && index < g_snd.max_3D_channels + 8) {
        alSource3f(oalGlob.sources[index], AL_POSITION, org[0], org[1], org[2]);
    }
}

void __cdecl SND_Stop2DChannel(int index) {
    if (index >= 0 && index < g_snd.max_2D_channels) {
        alSourceStop(oalGlob.sources[index]);
        SND_ResetChannelInfo(index);
        SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
    }
}

void __cdecl SND_Pause2DChannel(int index) {
    if (index >= 0 && index < g_snd.max_2D_channels) alSourcePause(oalGlob.sources[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause2DChannel(int index, int timeshift) {
    if (index >= 0 && index < g_snd.max_2D_channels) alSourcePlay(oalGlob.sources[index]);
    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is2DChannelFree(int index) {
    if (index < 0 || index >= g_snd.max_2D_channels) return true;
    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_Stop3DChannel(int index) {
    if (index >= 8 && index < g_snd.max_3D_channels + 8) {
        alSourceStop(oalGlob.sources[index]);
        SND_ResetChannelInfo(index);
        SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
    }
}

void __cdecl SND_Pause3DChannel(int index) {
    if (index >= g_snd.max_2D_channels && index < g_snd.max_2D_channels + g_snd.max_3D_channels) 
        alSourcePause(oalGlob.sources[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause3DChannel(int index, int timeshift) {
    if (index >= g_snd.max_2D_channels && index < g_snd.max_2D_channels + g_snd.max_3D_channels) 
        alSourcePlay(oalGlob.sources[index]);
    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is3DChannelFree(int index) {
    if (index < 8 || index >= g_snd.max_3D_channels + 8) return true;
    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_StopStreamChannel(int index) {
    if (index >= 40 && index < g_snd.max_stream_channels + 40) {
        int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
        OalStream* stream = &oalGlob.streams[localIdx];
        
        if (!stream->active) return;

        alSourceStop(stream->source);
        alSourcei(stream->source, AL_BUFFER, 0);

        if (stream->streamType == 1) drmp3_uninit(&stream->mp3Decoder);
        else if (stream->streamType == 2) drwav_uninit(&stream->wavDecoder);
        
        stream->streamType = 0; 

        if (stream->fileHandle) {
            FS_FCloseFile(stream->fileHandle);
            stream->fileHandle = 0;
        }

        stream->active = false;
        SND_ResetChannelInfo(index);
        SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
    }
}

void __cdecl SND_PauseStreamChannel(int index) {
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    if (localIdx >= 0 && localIdx < g_snd.max_stream_channels) 
        alSourcePause(oalGlob.streams[localIdx].source);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_UnpauseStreamChannel(int index, int timeshift) {
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    if (localIdx >= 0 && localIdx < g_snd.max_stream_channels) {
        alSourcePlay(oalGlob.streams[localIdx].source);
    }
    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_IsStreamChannelFree(int index) { 
    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

int __cdecl SND_StartAlias2DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    int entchannel = (startAliasInfo->alias0->flags & 0x3F00) >> 8;
    if (!SND_HasFreeVoice(entchannel)) return -1;
    
    int index = SND_FindFree2DChannel(startAliasInfo, entchannel);
    if (pChannel) *pChannel = index;
    if (index < 0) return -1;

    ALuint source = oalGlob.sources[index];
    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, sound->oalBuffer);

    float realVolume = startAliasInfo->volume * g_snd.volume * g_snd.channelvol->channelvol[entchannel].volume;
    alSourcef(source, AL_GAIN, realVolume);

    float pitch = startAliasInfo->pitch;
    if (startAliasInfo->timescale) pitch *= g_snd.timescale;
    alSourcef(source, AL_PITCH, pitch);

    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcei(source, AL_LOOPING, (startAliasInfo->alias0->flags & 1) != 0 ? AL_TRUE : AL_FALSE);

    if (!startAliasInfo->startDelay && (!g_snd.paused || !g_snd.pauseSettings[entchannel])) {
        alSourcePlay(source);
    }

    int total_msec = (sound->info.samples * 1000) / sound->info.rate;
    int start_msec = 0;

    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, total_msec, start_msec, SFLS_LOADED);
    
    int playbackId = SND_AcquirePlaybackId(index, total_msec);
    if (playbackId != -1) SND_AddVoice(entchannel);
    
    return playbackId;
}

int __cdecl SND_StartAlias3DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    int entchannel = (startAliasInfo->alias0->flags & 0x3F00) >> 8;
    if (!SND_HasFreeVoice(entchannel)) return -1;
    
    int index = SND_FindFree3DChannel(startAliasInfo, entchannel);
    if (pChannel) *pChannel = index;
    if (index < 0) return -1;

    ALuint source = oalGlob.sources[index];
    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, sound->oalBuffer);

    alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
    SND_Set3DPosition(index, startAliasInfo->org);
    alSourcef(source, AL_REFERENCE_DISTANCE, startAliasInfo->alias0->distMin);
    alSourcef(source, AL_MAX_DISTANCE, startAliasInfo->alias0->distMax);

    float realVolume = startAliasInfo->volume * g_snd.volume * g_snd.channelvol->channelvol[entchannel].volume;
    alSourcef(source, AL_GAIN, realVolume);

    float pitch = startAliasInfo->pitch;
    if (startAliasInfo->timescale) pitch *= g_snd.timescale;
    alSourcef(source, AL_PITCH, pitch);

    bool isLooping = ((startAliasInfo->alias0->flags & 1) != 0);
    alSourcei(source, AL_LOOPING, isLooping ? AL_TRUE : AL_FALSE);

    if (!startAliasInfo->startDelay && (!g_snd.paused || !g_snd.pauseSettings[entchannel])) {
        alSourcePlay(source);
    }

    int total_msec = (sound->info.samples * 1000) / sound->info.rate;
    int start_msec = 0; 

    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, total_msec, start_msec, SFLS_LOADED);
    
    int playbackId = SND_AcquirePlaybackId(index, total_msec);
    if (playbackId != -1) SND_AddVoice(entchannel);
    
    return playbackId;
}

int __cdecl SND_StartAliasStreamOnChannel(SndStartAliasInfo* startAliasInfo, int index)
{
    int entchannel = (startAliasInfo->alias0->flags & 0x3F00) >> 8;
    if (!SND_HasFreeVoice(entchannel)) return -1;

    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    OalStream* stream = &oalGlob.streams[localIdx];

    if (stream->active && stream->fileHandle) {
        alSourceStop(stream->source);
        alSourcei(stream->source, AL_BUFFER, 0);
        
        if (stream->streamType == 1) drmp3_uninit(&stream->mp3Decoder);
        else if (stream->streamType == 2) drwav_uninit(&stream->wavDecoder);
        stream->streamType = 0;
        
        FS_FCloseFile(stream->fileHandle);
        stream->active = false;
    }

    char filename[128], realname[256];
    Com_GetSoundFileName(startAliasInfo->alias0, filename, 128);
    Com_sprintf(realname, 256, "sound/%s", filename);

    if (FS_FOpenFileRead(realname, &stream->fileHandle) < 0) {
        Com_PrintError(1, "AUDIO ERROR: Couldn't open stream '%s'\n", realname);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    stream->streamType = 0;
    uint64_t totalFrames = 0;
    
    int nameLen = strlen(realname);
    bool isMp3Ext = (nameLen > 4 && _stricmp(&realname[nameLen - 4], ".mp3") == 0);

    // Init decoders. dr_mp3/dr_wav inherently check for EOF & calculate frame counts autonomously
    if (isMp3Ext) {
        if (drmp3_init(&stream->mp3Decoder, VFS_Read_Global, VFS_Seek_Global, VFS_Tell_Global, NULL, (void*)(intptr_t)stream->fileHandle, NULL)) {
            stream->streamType = 1;
            totalFrames = drmp3_get_pcm_frame_count(&stream->mp3Decoder);
        } else {
            FS_Seek(stream->fileHandle, 0, 2); // Rewind (SEEK_SET) for fallback
            if (drwav_init(&stream->wavDecoder, (drwav_read_proc)VFS_Read_Global, (drwav_seek_proc)VFS_Seek_Global, (drwav_tell_proc)VFS_Tell_Global, (void*)(intptr_t)stream->fileHandle, NULL)) {
                stream->streamType = 2;
                totalFrames = stream->wavDecoder.totalPCMFrameCount;
            }
        }
    } else {
        if (drwav_init(&stream->wavDecoder, (drwav_read_proc)VFS_Read_Global, (drwav_seek_proc)VFS_Seek_Global, (drwav_tell_proc)VFS_Tell_Global, (void*)(intptr_t)stream->fileHandle, NULL)) {
            stream->streamType = 2;
            totalFrames = stream->wavDecoder.totalPCMFrameCount;
        } else {
            FS_Seek(stream->fileHandle, 0, 2); // Rewind (SEEK_SET) for fallback
            if (drmp3_init(&stream->mp3Decoder, VFS_Read_Global, VFS_Seek_Global, VFS_Tell_Global, NULL, (void*)(intptr_t)stream->fileHandle, NULL)) {
                stream->streamType = 1;
                totalFrames = drmp3_get_pcm_frame_count(&stream->mp3Decoder);
            }
        }
    }

    if (stream->streamType == 1) {
        stream->rate = stream->mp3Decoder.sampleRate;
        stream->format = (stream->mp3Decoder.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    } 
    else if (stream->streamType == 2) {
        stream->rate = stream->wavDecoder.sampleRate;
        stream->format = (stream->wavDecoder.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        if (totalFrames == 0) totalFrames = 5 * stream->rate; // Failsafe
    } 
    else {
        FS_FCloseFile(stream->fileHandle);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    stream->isLooping = ((startAliasInfo->alias0->flags & 1) != 0);

    // Initial Buffer Queueing
    for (int i = 0; i < NUM_STREAM_BUFFERS; i++) {
        int bytesPerFrame = (stream->format == AL_FORMAT_STEREO16 ? 4 : 2);
        int framesToRead = STREAM_BUFFER_SIZE / bytesPerFrame;
        
        uint8_t chunkBuffer[STREAM_BUFFER_SIZE];
        uint64_t framesRead = 0;
        
        if (stream->streamType == 1) framesRead = drmp3_read_pcm_frames_s16(&stream->mp3Decoder, framesToRead, (drmp3_int16*)chunkBuffer);
        else if (stream->streamType == 2) framesRead = drwav_read_pcm_frames_s16(&stream->wavDecoder, framesToRead, (drwav_int16*)chunkBuffer);

        if (framesRead == 0) {
            if (stream->isLooping) {
                // Seek back gracefully instead of destroying decoders
                if (stream->streamType == 1) drmp3_seek_to_pcm_frame(&stream->mp3Decoder, 0);
                else if (stream->streamType == 2) drwav_seek_to_pcm_frame(&stream->wavDecoder, 0);
                i--; // Retry grabbing data for this buffer
                continue; 
            } else {
                break; // Hardware handles partial queues gracefully
            }
        }

        int actualBytesRead = (int)(framesRead * bytesPerFrame);
        alBufferData(stream->buffers[i], stream->format, chunkBuffer, actualBytesRead, stream->rate);
        alSourceQueueBuffers(stream->source, 1, &stream->buffers[i]);
    }

    // --- Spatialization support for Stream Channels ---
    bool is3D = SND_IsAliasChannel3D(entchannel);
    alSourcei(stream->source, AL_SOURCE_RELATIVE, is3D ? AL_FALSE : AL_TRUE);
    
    if (is3D) {
        alSourcef(stream->source, AL_REFERENCE_DISTANCE, startAliasInfo->alias0->distMin);
        alSourcef(stream->source, AL_MAX_DISTANCE, startAliasInfo->alias0->distMax);
        alSource3f(stream->source, AL_POSITION, startAliasInfo->org[0], startAliasInfo->org[1], startAliasInfo->org[2]);
    } else {
        alSource3f(stream->source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    }

    float realVolume = startAliasInfo->volume * g_snd.volume * g_snd.channelvol->channelvol[entchannel].volume;
    alSourcef(stream->source, AL_GAIN, realVolume);
    alSourcei(stream->source, AL_LOOPING, AL_FALSE); // Queue handles looping manually
    alSourcePlay(stream->source);
    stream->active = true;

    int total_msec = (totalFrames * 1000) / stream->rate;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, (stream->format == AL_FORMAT_STEREO16 ? 2 : 1), stream->rate, total_msec, 0, SFLS_LOADED);

    int playbackId = SND_AcquirePlaybackId(index, total_msec);
    if (playbackId != -1) SND_AddVoice(entchannel);

    return playbackId;
}

void __cdecl SND_SetRoomtype(int roomtype) {}
void __cdecl SND_UpdateEqs() {}
void __cdecl SND_SetEqParams(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type, float gain, float freq, float q) {}
void __cdecl SND_SetEqType(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type) {}
void __cdecl SND_SetEqFreq(uint32_t entchannel, int eqIndex, uint32_t band, float freq) {}
void __cdecl SND_SetEqGain(uint32_t entchannel, int eqIndex, uint32_t band, float gain) {}
void __cdecl SND_SetEqQ(uint32_t entchannel, int eqIndex, uint32_t band, float q) {}
void __cdecl SND_DisableEq(uint32_t entchannel, int eqIndex, uint32_t band) {}
void __cdecl SND_SaveEq(MemoryFile *memFile) {}
void __cdecl SND_RestoreEq(MemoryFile *memFile) {}
void __cdecl SND_PrintEqParams() {}

double __cdecl SND_Get2DChannelVolume(int index) {
    if (index < 0 || index >= g_snd.max_2D_channels) return 0.0;
    
    float gain = 0.0f;
    alGetSourcef(oalGlob.sources[index], AL_GAIN, &gain);
    
    if (g_snd.chaninfo[index].soundFileInfo.srcChannelCount == 2) return (double)gain;
    return (double)(gain * 2.0f);
}

void __cdecl SND_Set2DChannelVolume(int index, float volume) {
    if (index < 0 || index >= g_snd.max_2D_channels) return;
    
    float finalVol = volume;
    if (g_snd.chaninfo[index].soundFileInfo.srcChannelCount == 1) finalVol *= 0.5f;
    alSourcef(oalGlob.sources[index], AL_GAIN, finalVol);
}

double __cdecl SND_Get3DChannelVolume(int index) {
    if (index < 8 || index >= g_snd.max_3D_channels + 8) return 0.0;
    
    float gain = 0.0f;
    alGetSourcef(oalGlob.sources[index], AL_GAIN, &gain);
    
    if (g_snd.chaninfo[index].soundFileInfo.srcChannelCount == 2) return (double)gain;
    return (double)(gain * 2.0f);
}

void __cdecl SND_Set3DChannelVolume(int index, float volume) {
    if (index < 8 || index >= g_snd.max_3D_channels + 8) return;
    
    float finalVol = volume;
    if (g_snd.chaninfo[index].soundFileInfo.srcChannelCount == 1) finalVol *= 0.5f;
    alSourcef(oalGlob.sources[index], AL_GAIN, finalVol);
}

double __cdecl SND_GetStreamChannelVolume(int index) {
    if (index < 40 || index >= g_snd.max_stream_channels + 40) return 0.0;
    
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    float gain = 0.0f;
    alGetSourcef(oalGlob.streams[localIdx].source, AL_GAIN, &gain);
    
    if (g_snd.chaninfo[index].soundFileInfo.srcChannelCount == 2) return (double)gain;
    return (double)(gain * 2.0f);
}

void __cdecl SND_SetStreamChannelVolume(int index, float volume) {
    if (index < 40 || index >= g_snd.max_stream_channels + 40) return;
    
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    float finalVol = volume;
    if (oalGlob.streams[localIdx].format == AL_FORMAT_MONO16) finalVol *= 0.5f;
    alSourcef(oalGlob.streams[localIdx].source, AL_GAIN, finalVol);
}

int __cdecl SND_Get2DChannelPlaybackRate(int index) {
    if (index < 0 || index >= g_snd.max_2D_channels) return 44100;
    ALuint source = oalGlob.sources[index];
    ALint buffer = 0;
    alGetSourcei(source, AL_BUFFER, &buffer);
    if (!buffer) return 44100;

    ALint baseFreq = 44100;
    alGetBufferi(buffer, AL_FREQUENCY, &baseFreq);
    ALfloat pitch = 1.0f;
    alGetSourcef(source, AL_PITCH, &pitch);
    return (int)(baseFreq * pitch);
}

void __cdecl SND_Set2DChannelPlaybackRate(int index, int rate) {
    if (index < 0 || index >= g_snd.max_2D_channels) return;
    ALuint source = oalGlob.sources[index];
    ALint buffer = 0;
    alGetSourcei(source, AL_BUFFER, &buffer);
    if (buffer) {
        ALint baseFreq = 44100;
        alGetBufferi(buffer, AL_FREQUENCY, &baseFreq);
        if (baseFreq > 0) alSourcef(source, AL_PITCH, (float)rate / (float)baseFreq);
    }
}

int __cdecl SND_Get3DChannelPlaybackRate(int index) {
    if (index < 8 || index >= g_snd.max_3D_channels + 8) return 44100;
    ALuint source = oalGlob.sources[index];
    ALint buffer = 0;
    alGetSourcei(source, AL_BUFFER, &buffer);
    if (!buffer) return 44100;

    ALint baseFreq = 44100;
    alGetBufferi(buffer, AL_FREQUENCY, &baseFreq);
    ALfloat pitch = 1.0f;
    alGetSourcef(source, AL_PITCH, &pitch);
    return (int)(baseFreq * pitch);
}

void __cdecl SND_Set3DChannelPlaybackRate(int index, int rate) {
    if (index < 8 || index >= g_snd.max_3D_channels + 8) return;
    ALuint source = oalGlob.sources[index];
    ALint buffer = 0;
    alGetSourcei(source, AL_BUFFER, &buffer);
    if (buffer) {
        ALint baseFreq = 44100;
        alGetBufferi(buffer, AL_FREQUENCY, &baseFreq);
        if (baseFreq > 0) alSourcef(source, AL_PITCH, (float)rate / (float)baseFreq);
    }
}

int __cdecl SND_GetStreamChannelPlaybackRate(int index) {
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    if (localIdx >= 0 && localIdx < g_snd.max_stream_channels && oalGlob.streams[localIdx].active) {
        ALfloat pitch = 1.0f;
        alGetSourcef(oalGlob.streams[localIdx].source, AL_PITCH, &pitch);
        return (int)(oalGlob.streams[localIdx].rate * pitch);
    }
    return 44100;
}

void __cdecl SND_SetStreamChannelPlaybackRate(int index, int rate) {
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    if (localIdx >= 0 && localIdx < g_snd.max_stream_channels && oalGlob.streams[localIdx].active) {
        if (oalGlob.streams[localIdx].rate > 0) alSourcef(oalGlob.streams[localIdx].source, AL_PITCH, (float)rate / (float)oalGlob.streams[localIdx].rate);
    }
}

void __cdecl SND_Update2DChannelReverb(int index) {}
void __cdecl SND_Update3DChannelReverb(int index) {}
void __cdecl SND_UpdateStreamChannelReverb(int index) {}

int __cdecl SND_Get2DChannelLength(int index) {
    if (index >= 0 && index < g_snd.max_2D_channels) return g_snd.chaninfo[index].totalMsec;
    return 0;
}

int __cdecl SND_Get3DChannelLength(int index) {
    if (index >= g_snd.max_2D_channels && index < g_snd.max_2D_channels + g_snd.max_3D_channels) return g_snd.chaninfo[index].totalMsec;
    return 0;
}

int __cdecl SND_GetStreamChannelLength(int index) {
    if (index >= g_snd.max_2D_channels + g_snd.max_3D_channels && index < g_snd.max_2D_channels + g_snd.max_3D_channels + g_snd.max_stream_channels) 
        return g_snd.chaninfo[index].totalMsec;
    return 0;
}

// -------------------------------------------------------------------------
// SAVE / LOAD STATES
// -------------------------------------------------------------------------

void __cdecl SND_Get2DChannelSaveInfo(int index, snd_save_2D_sample_t *info)
{
    if (index < 0 || index >= g_snd.max_2D_channels || !info) return;
    
    ALuint source = oalGlob.sources[index];
    float secOffset = 0.0f;
    alGetSourcef(source, AL_SEC_OFFSET, &secOffset);
    float totalSec = (float)g_snd.chaninfo[index].totalMsec / 1000.0f;
    info->fraction = (totalSec > 0.0f) ? (double)(secOffset / totalSec) : 0.0;
    info->pitch = g_snd.chaninfo[index].pitch;

    float volume = 0.0f;
    alGetSourcef(source, AL_GAIN, &volume);
    info->volume = (g_snd.volume == 0.0f) ? g_snd.chaninfo[index].basevolume : (volume / g_snd.volume);
}

void __cdecl SND_Set2DChannelFromSaveInfo(int index, snd_save_2D_sample_t *info)
{
    if (index >= 0 && index < g_snd.max_2D_channels && info) SND_Set2DChannelVolume(index, info->volume * g_snd.volume);
}

void __cdecl SND_Get3DChannelSaveInfo(int index, snd_save_3D_sample_t *info)
{
    if (index < 8 || index >= g_snd.max_3D_channels + 8 || !info) return;
    
    ALuint source = oalGlob.sources[index];
    float secOffset = 0.0f;
    alGetSourcef(source, AL_SEC_OFFSET, &secOffset);
    float totalSec = (float)g_snd.chaninfo[index].totalMsec / 1000.0f;
    info->fraction = (totalSec > 0.0f) ? (double)(secOffset / totalSec) : 0.0;
    info->pitch = g_snd.chaninfo[index].pitch;

    float volume = 0.0f;
    alGetSourcef(source, AL_GAIN, &volume);
    info->volume = (g_snd.volume == 0.0f) ? g_snd.chaninfo[index].basevolume : (volume / g_snd.volume);
    alGetSource3f(source, AL_POSITION, &info->org[0], &info->org[1], &info->org[2]);
}

void __cdecl SND_GetStreamChannelSaveInfo(int index, snd_save_stream_t *info)
{
    if (index < 40 || index >= g_snd.max_stream_channels + 40 || !info) return;
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    OalStream* stream = &oalGlob.streams[localIdx];

    info->fraction = 0.0; // Exact fraction relies on dr_libs positioning, defaulting to 0 for save state stability

    ALfloat pitch = 1.0f;
    alGetSourcef(stream->source, AL_PITCH, &pitch);
    int rate = (int)(stream->rate * pitch);
    if (g_snd.chaninfo[index].timescale && g_snd.timescale > 0.0f) rate = (int)((float)rate / g_snd.timescale);
    info->rate = rate;

    info->basevolume = g_snd.chaninfo[index].basevolume;
    float volume = 0.0f;
    alGetSourcef(stream->source, AL_GAIN, &volume);
    info->volume = (g_snd.volume == 0.0f) ? g_snd.chaninfo[index].basevolume : (volume / g_snd.volume);

    info->org[0] = g_snd.chaninfo[index].org[0];
    info->org[1] = g_snd.chaninfo[index].org[1];
    info->org[2] = g_snd.chaninfo[index].org[2];
}

void __cdecl SND_SetStreamChannelFromSaveInfo(int index, snd_save_stream_t *info)
{
    if (index >= 40 && index < g_snd.max_stream_channels + 40 && info) SND_SetStreamChannelVolume(index, info->volume * g_snd.volume);
}

int __cdecl SND_GetSoundFileSize(uint32_t *pSoundFile) { return pSoundFile ? pSoundFile[2] : 0; }

void __cdecl SND_DriverPostUpdate()
{
    const float* origin = g_snd.listeners[0].orient.origin;
    const float* axis = g_snd.listeners[0].orient.axis[0];
    const float* up = g_snd.listeners[0].orient.axis[2];
    alListener3f(AL_POSITION, origin[0], origin[1], origin[2]);
    
    float orientation[6] = { axis[0], axis[1], axis[2], up[0], up[1], up[2] };
    alListenerfv(AL_ORIENTATION, orientation);
}

void __cdecl SND_Update2DChannel(int i, int frametime)
{
    if (i < 0 || i >= g_snd.max_2D_channels) return;
    snd_channel_info_t *chaninfo = &g_snd.chaninfo[i];
    
    if (!chaninfo->paused && chaninfo->alias0)
    {
        ALint state;
        alGetSourcei(oalGlob.sources[i], AL_SOURCE_STATE, &state);
        
        bool isHardwareStopped = (!chaninfo->startDelay && state == AL_STOPPED);
        bool chainTimeReached = (chaninfo->alias0->chainAliasName && (chaninfo->totalMsec + chaninfo->startTime - g_snd.time <= 0));

        if (isHardwareStopped || chainTimeReached) SND_StopChannelAndPlayChainAlias(i);
        else
        {
            float volume = chaninfo->basevolume;
            if (g_snd.slaveLerp != 0.0 && !chaninfo->master && (chaninfo->alias0->flags & 4) != 0) {
                volume = SND_GetLerpedSlavePercentage(chaninfo->alias0->slavePercentage) * volume;
            }
            int entchannel = (chaninfo->alias0->flags & 0x3F00) >> 8;
            float finalVol = volume * g_snd.channelvol->channelvol[entchannel].volume * g_snd.volume;
            SND_Set2DChannelVolume(i, finalVol);
        }
    }
}

void __cdecl SND_Update3DChannel(int i, int frametime)
{
    if (i < 8 || i >= g_snd.max_3D_channels + 8) return;
    snd_channel_info_t *chaninfo = &g_snd.chaninfo[i];
    
    if (!chaninfo->paused && chaninfo->alias0)
    {
        ALint state;
        alGetSourcei(oalGlob.sources[i], AL_SOURCE_STATE, &state);
        
        bool isHardwareStopped = (!chaninfo->startDelay && state == AL_STOPPED);
        bool chainTimeReached = (chaninfo->alias0->chainAliasName && (chaninfo->totalMsec + chaninfo->startTime - g_snd.time <= 0));

        if (isHardwareStopped || chainTimeReached) SND_StopChannelAndPlayChainAlias(i);
        else
        {
            float org[3];
            SND_GetCurrent3DPosition(chaninfo->sndEnt, chaninfo->offset, org);
            SND_Set3DPosition(i, org);

            float volume = chaninfo->basevolume;
            if (g_snd.slaveLerp != 0.0 && !chaninfo->master && (chaninfo->alias0->flags & 4) != 0) {
                volume = SND_GetLerpedSlavePercentage(chaninfo->alias0->slavePercentage) * volume;
            }
            int entchannel = (chaninfo->alias0->flags & 0x3F00) >> 8;
            float finalVol = volume * g_snd.channelvol->channelvol[entchannel].volume * g_snd.volume;
            alSourcef(oalGlob.sources[i], AL_GAIN, finalVol);
        }
    }
}

void __cdecl SND_UpdateStreamChannel(int i, int frametime)
{
    if (i < 40 || i >= g_snd.max_stream_channels + 40) return;
    
    snd_channel_info_t *chaninfo = &g_snd.chaninfo[i];
    int localIdx = i - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    OalStream* stream = &oalGlob.streams[localIdx];

    if (!stream->active) return;

    if (!chaninfo->paused && chaninfo->alias0)
    {
        // 1. Process the Ring Buffer Safely
        ALint processed;
        alGetSourcei(stream->source, AL_BUFFERS_PROCESSED, &processed);

        while (processed > 0) {
            ALuint bufferId;
            alSourceUnqueueBuffers(stream->source, 1, &bufferId);

            int bytesPerFrame = (stream->format == AL_FORMAT_STEREO16 ? 4 : 2);
            int framesToRead = STREAM_BUFFER_SIZE / bytesPerFrame;
            
            uint8_t chunkBuffer[STREAM_BUFFER_SIZE];
            uint64_t framesRead = 0;

            if (stream->streamType == 1) framesRead = drmp3_read_pcm_frames_s16(&stream->mp3Decoder, framesToRead, (drmp3_int16*)chunkBuffer);
            else if (stream->streamType == 2) framesRead = drwav_read_pcm_frames_s16(&stream->wavDecoder, framesToRead, (drwav_int16*)chunkBuffer);

            // Reached EOF naturally
            if (framesRead == 0) {
                if (stream->isLooping) {
                    if (stream->streamType == 1) drmp3_seek_to_pcm_frame(&stream->mp3Decoder, 0);
                    else if (stream->streamType == 2) drwav_seek_to_pcm_frame(&stream->wavDecoder, 0);
                    
                    // Trigger a re-read immediately
                    if (stream->streamType == 1) framesRead = drmp3_read_pcm_frames_s16(&stream->mp3Decoder, framesToRead, (drmp3_int16*)chunkBuffer);
                    else if (stream->streamType == 2) framesRead = drwav_read_pcm_frames_s16(&stream->wavDecoder, framesToRead, (drwav_int16*)chunkBuffer);
                } 
            }

            // Queue data if available (this naturally terminates OpenAL queues upon true EOF)
            if (framesRead > 0) {
                int actualBytesRead = (int)(framesRead * bytesPerFrame);
                alBufferData(bufferId, stream->format, chunkBuffer, actualBytesRead, stream->rate);
                alSourceQueueBuffers(stream->source, 1, &bufferId);
            }
            
            processed--;
        }

        // 2. Hardware state monitoring & Kickstart Fix
        ALint state, queued;
        alGetSourcei(stream->source, AL_SOURCE_STATE, &state);
        alGetSourcei(stream->source, AL_BUFFERS_QUEUED, &queued);
        
        if ((state == AL_STOPPED || state == AL_INITIAL) && queued > 0) {
            alSourcePlay(stream->source);
        }

        // 3. True Engine Teardown
        bool chainTimeReached = (chaninfo->alias0->chainAliasName && (chaninfo->totalMsec + chaninfo->startTime - g_snd.time <= 0));
        
        if ((state == AL_STOPPED && queued == 0) || chainTimeReached) {
            SND_StopChannelAndPlayChainAlias(i);
        } else {
            // Keep volume synced dynamically
            float volume = chaninfo->basevolume;
            if (g_snd.slaveLerp != 0.0 && !chaninfo->master && (chaninfo->alias0->flags & 4) != 0) {
                volume = SND_GetLerpedSlavePercentage(chaninfo->alias0->slavePercentage) * volume;
            }
            int entchannel = (chaninfo->alias0->flags & 0x3F00) >> 8;
            float finalVol = volume * g_snd.channelvol->channelvol[entchannel].volume * g_snd.volume;
            
            // MSS Mono Mixing Match
            if (stream->format == AL_FORMAT_MONO16) finalVol *= 0.5f; 
            alSourcef(stream->source, AL_GAIN, finalVol);

            // Spatialization Updates for Streams (Like 3D entity voice lines)
            if (SND_IsAliasChannel3D(entchannel)) {
                SND_GetCurrent3DPosition(chaninfo->sndEnt, chaninfo->offset, chaninfo->org);
                alSource3f(stream->source, AL_POSITION, chaninfo->org[0], chaninfo->org[1], chaninfo->org[2]);
            }
        }
    }
}

#ifdef KISAK_SP
void SND_SetEqLerp(double lerp) {}
#endif

struct _DIG_DRIVER* __cdecl MSS_GetDriver(void) { 
    return nullptr; 
}

#endif // USE_OPENAL