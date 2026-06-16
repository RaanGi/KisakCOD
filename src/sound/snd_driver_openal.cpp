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

double __cdecl GetWetLevel(const snd_alias_t *pAlias)
{
    if (g_snd.effect->wetlevel < 0.0 || g_snd.effect->wetlevel > 1.0)
        return 0.0; // Failsafe
    
    if (!pAlias)
        return g_snd.effect->wetlevel; // Master level query
        
    // CoD4 uses flag 0x10 to specify "No Reverb" for specific sounds (like UI clicks)
    if (!snd_enableReverb->current.enabled || (pAlias->flags & 0x10) != 0)
        return 0.0f;
        
    return g_snd.effect->wetlevel;
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

    if (alcIsExtensionPresent(oalGlob.device, "ALC_EXT_EFX")) {
        oalGlob.efxSupported = true;
        
        // 2. Load Function Pointers
        alGenAuxiliaryEffectSlots = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
        alDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
        alGenEffects = (LPALGENEFFECTS)alGetProcAddress("alGenEffects");
        alDeleteEffects = (LPALDELETEEFFECTS)alGetProcAddress("alDeleteEffects");
        alAuxiliaryEffectSloti = (LPALAUXILIARYEFFECTSLOTI)alGetProcAddress("alAuxiliaryEffectSloti");
        alEffecti = (LPALEFFECTI)alGetProcAddress("alEffecti");
        alEffectf = (LPALEFFECTF)alGetProcAddress("alEffectf");
        alGenFilters = (LPALGENFILTERS)alGetProcAddress("alGenFilters");
        alFilteri = (LPALFILTERI)alGetProcAddress("alFilteri");
        alFilterf = (LPALFILTERF)alGetProcAddress("alFilterf");
        alDeleteFilters = (LPALDELETEFILTERS)alGetProcAddress("alDeleteFilters");

        // 3. Generate Hardware DSP Busses (One for each entchannel)
        alGenAuxiliaryEffectSlots(64, oalGlob.eqAuxSlots);
        alGenEffects(64, oalGlob.eqEffects);

        alGenFilters(1, &oalGlob.muteFilter);
        alFilteri(oalGlob.muteFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(oalGlob.muteFilter, AL_LOWPASS_GAIN, 0.0f); // 0 volume = completely muted
        
        for (int i = 0; i < 64; i++) oalGlob.eqDirty[i] = true;


        // Create the Master Reverb Bus
        alGenAuxiliaryEffectSlots(1, &oalGlob.reverbAuxSlot);
        alGenEffects(1, &oalGlob.reverbEffect);
        
        // Set effect type to Reverb
        alEffecti(oalGlob.reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        alAuxiliaryEffectSloti(oalGlob.reverbAuxSlot, AL_EFFECTSLOT_EFFECT, oalGlob.reverbEffect);
        oalGlob.currentRoomType = -1;

        for (int i = 0; i < 64; i++) {
            // Set the effect type to a standard Equalizer
            alEffecti(oalGlob.eqEffects[i], AL_EFFECT_TYPE, AL_EFFECT_EQUALIZER);
            
            // Bind the EQ effect to the Aux Slot
            alAuxiliaryEffectSloti(oalGlob.eqAuxSlots[i], AL_EFFECTSLOT_EFFECT, oalGlob.eqEffects[i]);
            oalGlob.eqActive[i] = false;
        }
        Com_Printf(9, "OpenAL EFX Extension Loaded Successfully.\n");
    } else {
        oalGlob.efxSupported = false;
        Com_Printf(9, "WARNING: OpenAL EFX not supported by hardware. EQ disabled.\n");
    }

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

    if (oalGlob.efxSupported) {
        alDeleteAuxiliaryEffectSlots(64, oalGlob.eqAuxSlots);
        alDeleteEffects(64, oalGlob.eqEffects);
        alDeleteFilters(1, &oalGlob.muteFilter);
        alDeleteAuxiliaryEffectSlots(1, &oalGlob.reverbAuxSlot);
        alDeleteEffects(1, &oalGlob.reverbEffect);
    }

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
        Com_PrintError(1, "AUDIO ERROR: Both MP3 and WAV decoders failed to parse stream '%s'. File may be unsupported.\n", realname);
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
    float pitch = startAliasInfo->pitch;
    if (startAliasInfo->timescale && g_snd.timescale > 0.0f) {
        pitch *= g_snd.timescale;
    }
    alSourcef(stream->source, AL_PITCH, pitch);
    alSourcePlay(stream->source);
    stream->active = true;

    int total_msec = (totalFrames * 1000) / stream->rate;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, (stream->format == AL_FORMAT_STEREO16 ? 2 : 1), stream->rate, total_msec, 0, SFLS_LOADED);

    int playbackId = SND_AcquirePlaybackId(index, total_msec);
    if (playbackId != -1) SND_AddVoice(entchannel);

    return playbackId;
}

// Helper to easily load EFX macros into an OpenAL effect
void LoadEFXReverbPreset(ALuint effect, const EFXEAXREVERBPROPERTIES* preset, float masterWetLevel) 
{
    // Apply preset properties (mapped to standard AL_EFFECT_REVERB)
    alEffectf(effect, AL_REVERB_DENSITY, preset->flDensity);
    alEffectf(effect, AL_REVERB_DIFFUSION, preset->flDiffusion);
    alEffectf(effect, AL_REVERB_GAIN, preset->flGain * masterWetLevel); // Apply Engine Wet Level here
    alEffectf(effect, AL_REVERB_GAINHF, preset->flGainHF);
    alEffectf(effect, AL_REVERB_DECAY_TIME, preset->flDecayTime);
    alEffectf(effect, AL_REVERB_DECAY_HFRATIO, preset->flDecayHFRatio);
    alEffectf(effect, AL_REVERB_REFLECTIONS_GAIN, preset->flReflectionsGain);
    alEffectf(effect, AL_REVERB_REFLECTIONS_DELAY, preset->flReflectionsDelay);
    alEffectf(effect, AL_REVERB_LATE_REVERB_GAIN, preset->flLateReverbGain);
    alEffectf(effect, AL_REVERB_LATE_REVERB_DELAY, preset->flLateReverbDelay);
    alEffectf(effect, AL_REVERB_AIR_ABSORPTION_GAINHF, preset->flAirAbsorptionGainHF);
    alEffectf(effect, AL_REVERB_ROOM_ROLLOFF_FACTOR, preset->flRoomRolloffFactor);
    alEffecti(effect, AL_REVERB_DECAY_HFLIMIT, preset->iDecayHFLimit);
}

void __cdecl SND_SetRoomtype(int roomtype)
{
    if (!oalGlob.efxSupported) return;
    
    // Only update the hardware if the room type actually changed to save CPU
    // (We also check if wetLevel changed, but for simplicity let's update if roomtype changes)
    if (roomtype == oalGlob.currentRoomType) return;
    oalGlob.currentRoomType = roomtype;

    // Map CoD4's EAX Room ID to OpenAL's EFX Presets
    EFXEAXREVERBPROPERTIES preset;
    switch (roomtype) {
        case 0:  preset = EFX_REVERB_PRESET_GENERIC; break;
        case 1:  preset = EFX_REVERB_PRESET_PADDEDCELL; break;
        case 2:  preset = EFX_REVERB_PRESET_ROOM; break;
        case 3:  preset = EFX_REVERB_PRESET_BATHROOM; break;
        case 4:  preset = EFX_REVERB_PRESET_LIVINGROOM; break;
        case 5:  preset = EFX_REVERB_PRESET_STONEROOM; break;
        case 6:  preset = EFX_REVERB_PRESET_AUDITORIUM; break;
        case 7:  preset = EFX_REVERB_PRESET_CONCERTHALL; break;
        case 8:  preset = EFX_REVERB_PRESET_CAVE; break;
        case 9:  preset = EFX_REVERB_PRESET_ARENA; break;
        case 10: preset = EFX_REVERB_PRESET_HANGAR; break;
        case 11: preset = EFX_REVERB_PRESET_CARPETEDHALLWAY; break;
        case 12: preset = EFX_REVERB_PRESET_HALLWAY; break;
        case 13: preset = EFX_REVERB_PRESET_STONECORRIDOR; break;
        case 14: preset = EFX_REVERB_PRESET_ALLEY; break;
        case 15: preset = EFX_REVERB_PRESET_FOREST; break;
        case 16: preset = EFX_REVERB_PRESET_CITY; break;
        case 17: preset = EFX_REVERB_PRESET_MOUNTAINS; break;
        case 18: preset = EFX_REVERB_PRESET_QUARRY; break;
        case 19: preset = EFX_REVERB_PRESET_PLAIN; break;
        case 20: preset = EFX_REVERB_PRESET_PARKINGLOT; break;
        case 21: preset = EFX_REVERB_PRESET_SEWERPIPE; break;
        case 22: preset = EFX_REVERB_PRESET_UNDERWATER; break;
        case 23: preset = EFX_REVERB_PRESET_DRUGGED; break;
        case 24: preset = EFX_REVERB_PRESET_DIZZY; break;
        case 25: preset = EFX_REVERB_PRESET_PSYCHOTIC; break;
        default: 
            Com_PrintError(1, "OPENAL: No preset for roomtype %d\n", roomtype);
            preset = EFX_REVERB_PRESET_GENERIC; break;
    }

    // Apply the preset to our effect, then load the effect into the Reverb Slot
    LoadEFXReverbPreset(oalGlob.reverbEffect, &preset, GetWetLevel(0));
    alAuxiliaryEffectSloti(oalGlob.reverbAuxSlot, AL_EFFECTSLOT_EFFECT, oalGlob.reverbEffect);
}

void __cdecl SND_UpdateEqs()
{
    if (!oalGlob.efxSupported) return;

    for (int entchannel = 0; entchannel < 64; ++entchannel)
    {
        if (!oalGlob.eqDirty[entchannel]) continue;

        ALuint effect = oalGlob.eqEffects[entchannel];
        bool hasActiveBands = false;

        // Reset the OpenAL EQ to a flat baseline before applying bands
        alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EQUALIZER);
        
        for (int band = 0; band < 3; ++band)
        {
            // Read from our new native OpenAL state buffer
            SndEqParams& eq = oalGlob.eqParams[0][band][entchannel];

            if (eq.enabled)
            {
                hasActiveBands = true;
                
                float gain = eq.gain; 
                float freq = eq.freq;
                float width = (eq.q > 0.0f) ? (1.0f / eq.q) : 1.0f;

                if (freq < 500.0f || eq.type == 0) {
                    alEffectf(effect, AL_EQUALIZER_LOW_GAIN, gain);
                    alEffectf(effect, AL_EQUALIZER_LOW_CUTOFF, freq);
                } 
                else if (freq > 4000.0f || eq.type == 2) {
                    alEffectf(effect, AL_EQUALIZER_HIGH_GAIN, gain);
                    alEffectf(effect, AL_EQUALIZER_HIGH_CUTOFF, freq);
                } 
                else {
                    if (band == 0) {
                        alEffectf(effect, AL_EQUALIZER_MID1_GAIN, gain);
                        alEffectf(effect, AL_EQUALIZER_MID1_CENTER, freq);
                        alEffectf(effect, AL_EQUALIZER_MID1_WIDTH, width);
                    } else if (band == 1) {
                        alEffectf(effect, AL_EQUALIZER_MID2_GAIN, gain);
                        alEffectf(effect, AL_EQUALIZER_MID2_CENTER, freq);
                        alEffectf(effect, AL_EQUALIZER_MID2_WIDTH, width);
                    } else {
                        // Band 2 shares MID2 if it's in the mid-range, but 0 and 1 are now separate
                        alEffectf(effect, AL_EQUALIZER_MID2_GAIN, gain);
                        alEffectf(effect, AL_EQUALIZER_MID2_CENTER, freq);
                        alEffectf(effect, AL_EQUALIZER_MID2_WIDTH, width);
                    }
                }
            }
        }

        // Apply to hardware bus
        if (hasActiveBands) {
            alAuxiliaryEffectSloti(oalGlob.eqAuxSlots[entchannel], AL_EFFECTSLOT_EFFECT, effect);
            oalGlob.eqActive[entchannel] = true;
        } else {
            alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_NULL);
            alAuxiliaryEffectSloti(oalGlob.eqAuxSlots[entchannel], AL_EFFECTSLOT_EFFECT, effect);
            oalGlob.eqActive[entchannel] = false;
        }
        oalGlob.eqDirty[entchannel] = false;
    }
}


void __cdecl SND_SetEqParams(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type, float gain, float freq, float q)
{
    if (entchannel >= 64 || band >= 3 || eqIndex >= 2) return;
    
    oalGlob.eqParams[eqIndex][band][entchannel].enabled = 1;
    oalGlob.eqParams[eqIndex][band][entchannel].gain = gain;
    oalGlob.eqParams[eqIndex][band][entchannel].freq = freq;
    oalGlob.eqParams[eqIndex][band][entchannel].q = q;
    oalGlob.eqParams[eqIndex][band][entchannel].type = type;
    oalGlob.eqDirty[entchannel] = true;
}


void __cdecl SND_SetEqType(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type) {
    if (entchannel >= 64 || band >= 3 || eqIndex >= 2) return;
    oalGlob.eqParams[eqIndex][band][entchannel].enabled = 1;
    oalGlob.eqParams[eqIndex][band][entchannel].type = type;
    oalGlob.eqDirty[entchannel] = true;
}

void __cdecl SND_SetEqFreq(uint32_t entchannel, int eqIndex, uint32_t band, float freq) {
    if (entchannel >= 64 || band >= 3 || eqIndex >= 2) return;
    oalGlob.eqParams[eqIndex][band][entchannel].enabled = 1;
    oalGlob.eqParams[eqIndex][band][entchannel].freq = freq;
    oalGlob.eqDirty[entchannel] = true;
}

void __cdecl SND_SetEqGain(uint32_t entchannel, int eqIndex, uint32_t band, float gain) {
    if (entchannel >= 64 || band >= 3 || eqIndex >= 2) return;
    oalGlob.eqParams[eqIndex][band][entchannel].enabled = 1;
    oalGlob.eqParams[eqIndex][band][entchannel].gain = gain;
    oalGlob.eqDirty[entchannel] = true;
}

void __cdecl SND_SetEqQ(uint32_t entchannel, int eqIndex, uint32_t band, float q) {
    if (entchannel >= 64 || band >= 3 || eqIndex >= 2) return;
    oalGlob.eqParams[eqIndex][band][entchannel].enabled = 1;
    oalGlob.eqParams[eqIndex][band][entchannel].q = q;
    oalGlob.eqDirty[entchannel] = true;
}

void __cdecl SND_DisableEq(uint32_t entchannel, int eqIndex, uint32_t band) {
    if (entchannel >= 64 || band >= 3 || eqIndex >= 2) return;
    oalGlob.eqParams[eqIndex][band][entchannel].enabled = 0;
    oalGlob.eqDirty[entchannel] = true;
}


void __cdecl SND_SaveEq(MemoryFile *memFile)
{
    for (int eqIndex = 0; eqIndex < 2; ++eqIndex) {
        for (int band = 0; band < 3; ++band) {
            for (int entchannel = 0; entchannel < 64; ++entchannel) {
                // memFile reads exactly 20 bytes, which our struct perfectly matches
                MemFile_WriteData(memFile, 20, &oalGlob.eqParams[eqIndex][band][entchannel]);
            }
        }
    }
}

void __cdecl SND_RestoreEq(MemoryFile *memFile)
{
    for (int eqIndex = 0; eqIndex < 2; ++eqIndex) {
        for (int band = 0; band < 3; ++band) {
            for (int entchannel = 0; entchannel < 64; ++entchannel) {
                MemFile_ReadData(memFile, 20, (uint8_t *)&oalGlob.eqParams[eqIndex][band][entchannel]);
            }
        }
    }
}

void __cdecl SND_PrintEqParams()
{
    Com_Printf(9, "Current OpenAL EQ Settings\n---------------\n");
    for (int entchannel = 0; entchannel < g_snd.entchannel_count; ++entchannel)
    {
        snd_entchannel_info_t *channelName = SND_GetEntChannelName(entchannel);
        Com_Printf(9, "+ %s\n", channelName->name);
        for (int eqIndex = 0; eqIndex < 2; ++eqIndex)
        {
            for (int band = 0; band < 3; ++band)
            {
                SndEqParams* p = &oalGlob.eqParams[eqIndex][band][entchannel];
                if (p->enabled) {
                    Com_Printf(9, "\tBand %i: Type %i | %f Hz | %f dB | %f q\n", 
                               band, p->type, p->freq, p->gain, p->q);
                }
            }
        }
    }
}

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

    // Add + 0.5f to force exact integer rounding, preventing drift
    return (int)((ALfloat)baseFreq * pitch + 0.5f);
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

    // Add + 0.5f to force exact integer rounding, preventing drift
    return (int)((ALfloat)baseFreq * pitch + 0.5f);
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
        return (int)((ALfloat)oalGlob.streams[localIdx].rate * pitch + 0.5f);
    }
    return 44100;
}

void __cdecl SND_SetStreamChannelPlaybackRate(int index, int rate) {
    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    if (localIdx >= 0 && localIdx < g_snd.max_stream_channels && oalGlob.streams[localIdx].active) {
        if (oalGlob.streams[localIdx].rate > 0) alSourcef(oalGlob.streams[localIdx].source, AL_PITCH, (float)rate / (float)oalGlob.streams[localIdx].rate);
    }
}

void __cdecl SND_Update2DChannelReverb(int index) 
{
    if (!oalGlob.efxSupported || index < 0 || index >= g_snd.max_2D_channels) return;
    
    snd_channel_info_t *chaninfo = &g_snd.chaninfo[index];
    if (chaninfo->paused || !chaninfo->alias0) return;

    ALuint source = oalGlob.sources[index];
    int entchannel = (chaninfo->alias0->flags & 0x3F00) >> 8;

    // ---- EQ / OCCLUSION (Send Index 0) ----
    if (entchannel < 64 && oalGlob.eqActive[entchannel]) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, oalGlob.eqAuxSlots[entchannel], 0, AL_FILTER_NULL);
        // Removed direct mute to avoid "dry" sound; signal now mixes processed and unprocessed
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
    }

    // ---- ENVIRONMENTAL REVERB (Send Index 1) ----
    float sourceWetLevel = GetWetLevel(chaninfo->alias0);
    if (sourceWetLevel > 0.0f) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, oalGlob.reverbAuxSlot, 1, AL_FILTER_NULL);
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
    }
}


void __cdecl SND_Update3DChannelReverb(int index) 
{
    if (!oalGlob.efxSupported || index < 8 || index >= g_snd.max_3D_channels + 8) return;
    
    snd_channel_info_t *chaninfo = &g_snd.chaninfo[index];
    if (chaninfo->paused || !chaninfo->alias0) return;

    ALuint source = oalGlob.sources[index];
    int entchannel = (chaninfo->alias0->flags & 0x3F00) >> 8;

    // ---- EQ / OCCLUSION (Send Index 0) ----
    if (entchannel < 64 && oalGlob.eqActive[entchannel]) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, oalGlob.eqAuxSlots[entchannel], 0, AL_FILTER_NULL);
        // Removed direct mute to avoid "dry" sound; signal now mixes processed and unprocessed
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
    }

    // ---- ENVIRONMENTAL REVERB (Send Index 1) ----
    float sourceWetLevel = GetWetLevel(chaninfo->alias0);
    if (sourceWetLevel > 0.0f) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, oalGlob.reverbAuxSlot, 1, AL_FILTER_NULL);
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
    }
}


void __cdecl SND_UpdateStreamChannelReverb(int index) 
{
    if (!oalGlob.efxSupported || index < 40 || index >= g_snd.max_stream_channels + 40) return;
    
    snd_channel_info_t *chaninfo = &g_snd.chaninfo[index];
    if (chaninfo->paused || !chaninfo->alias0) return;

    int localIdx = index - (g_snd.max_2D_channels + g_snd.max_3D_channels);
    OalStream* stream = &oalGlob.streams[localIdx];
    
    if (!stream->active) return;

    ALuint source = stream->source;
    int entchannel = (chaninfo->alias0->flags & 0x3F00) >> 8;

    // ---- EQ / OCCLUSION (Send Index 0) ----
    if (entchannel < 64 && oalGlob.eqActive[entchannel]) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, oalGlob.eqAuxSlots[entchannel], 0, AL_FILTER_NULL);
        // Removed direct mute to avoid "dry" sound; signal now mixes processed and unprocessed
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
    }

    // ---- ENVIRONMENTAL REVERB (Send Index 1) ----
    float sourceWetLevel = GetWetLevel(chaninfo->alias0);
    if (sourceWetLevel > 0.0f) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, oalGlob.reverbAuxSlot, 1, AL_FILTER_NULL);
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
    }
}


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
        // 1. Subtract elapsed time from the delay!
        if (chaninfo->startDelay > 0) {
            chaninfo->startDelay -= frametime;
            // The exact millisecond the delay finishes, kickstart the hardware!
            if (chaninfo->startDelay <= 0) {
                alSourcePlay(oalGlob.sources[i]);
            }
        }

        ALint state;
        alGetSourcei(oalGlob.sources[i], AL_SOURCE_STATE, &state);

        // 2. Only consider the sound finished if the delay is over AND it stopped playing
        bool isHardwareStopped = (chaninfo->startDelay <= 0 && state == AL_STOPPED);
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
        if (chaninfo->startDelay > 0) {
            chaninfo->startDelay -= frametime;
            if (chaninfo->startDelay <= 0) {
                alSourcePlay(oalGlob.sources[i]);
            }
        }

        ALint state;
        alGetSourcei(oalGlob.sources[i], AL_SOURCE_STATE, &state);

        bool isHardwareStopped = (chaninfo->startDelay <= 0 && state == AL_STOPPED);
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

void __cdecl SND_CinematicInitAudio(int rate, int channels, int bits)
{
    if (!oalGlob.context) return;

    oalGlob.cinematicFormat = AL_FORMAT_MONO16; 
    if (channels == 1 && bits == 8)       oalGlob.cinematicFormat = AL_FORMAT_MONO8;
    else if (channels == 1 && bits == 16) oalGlob.cinematicFormat = AL_FORMAT_MONO16;
    else if (channels == 2 && bits == 8)  oalGlob.cinematicFormat = AL_FORMAT_STEREO8;
    else if (channels == 2 && bits == 16) oalGlob.cinematicFormat = AL_FORMAT_STEREO16;
    
    oalGlob.cinematicRate = rate;
    oalGlob.cinematicWriteIdx = 0; // Reset the rotating index

    alGenSources(1, &oalGlob.cinematicSource);
    alGenBuffers(NUM_STREAM_BUFFERS, oalGlob.cinematicBuffers);

    alSourcei(oalGlob.cinematicSource, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(oalGlob.cinematicSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcei(oalGlob.cinematicSource, AL_LOOPING, AL_FALSE);
    
    // Force full volume to guarantee we hear it during testing
    alSourcef(oalGlob.cinematicSource, AL_GAIN, 1.0f);

    oalGlob.cinematicActive = true;
}

void __cdecl SND_CinematicPushAudio(const uint8_t* pcmData, size_t bytes)
{
    if (!oalGlob.cinematicActive || !pcmData || bytes == 0) return;

    // 1. Unqueue all buffers that have finished playing
    ALint processed;
    alGetSourcei(oalGlob.cinematicSource, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0) {
        ALuint bufferId;
        alSourceUnqueueBuffers(oalGlob.cinematicSource, 1, &bufferId);
        processed--;
    }

    // 2. Safely push new data using the rotating write index
    ALint queued;
    alGetSourcei(oalGlob.cinematicSource, AL_BUFFERS_QUEUED, &queued);
    
    if (queued < NUM_STREAM_BUFFERS) {
        // Safely pick the next buffer in the ring
        ALuint freeBuffer = oalGlob.cinematicBuffers[oalGlob.cinematicWriteIdx];
        
        // Rotate the index forward, wrapping around back to 0 if we hit the limit
        oalGlob.cinematicWriteIdx = (oalGlob.cinematicWriteIdx + 1) % NUM_STREAM_BUFFERS;

        alBufferData(freeBuffer, oalGlob.cinematicFormat, pcmData, bytes, oalGlob.cinematicRate);
        alSourceQueueBuffers(oalGlob.cinematicSource, 1, &freeBuffer);
    }

    // 3. Kickstart playback if the video decoder lagged and OpenAL starved
    ALint state;
    alGetSourcei(oalGlob.cinematicSource, AL_SOURCE_STATE, &state);
    alGetSourcei(oalGlob.cinematicSource, AL_BUFFERS_QUEUED, &queued);
    
    if ((state == AL_STOPPED || state == AL_INITIAL) && queued > 0) {
        alSourcePlay(oalGlob.cinematicSource);
    }
}

void __cdecl SND_CinematicStopAudio()
{
    if (!oalGlob.cinematicActive) return;

    alSourceStop(oalGlob.cinematicSource);
    alSourcei(oalGlob.cinematicSource, AL_BUFFER, 0);

    alDeleteSources(1, &oalGlob.cinematicSource);
    alDeleteBuffers(NUM_STREAM_BUFFERS, oalGlob.cinematicBuffers);

    oalGlob.cinematicActive = false;
}

#endif // USE_OPENAL