// 包含头文件
#include <windows.h>
extern "C" {
#include "coreplayer.h"
#include "../corerender/corerender.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

inline void TRACE(LPCSTR lpszFormat, ...)
{
    va_list args;
    char    buf[512];

    va_start(args, lpszFormat);
    _vsnprintf(buf, sizeof(buf), lpszFormat, args);
    OutputDebugStringA(buf);
    va_end(args);
}

// 内部常量定义
#define avcodec_decode_video avcodec_decode_video2
#define avcodec_decode_audio avcodec_decode_audio4

// 内部类型定义
typedef struct
{
    AVFormatContext *pAVFormatContext;
    AVCodecContext  *pVideoCodecContext;
    AVCodecContext  *pAudioCodecContext;
    int              iVideoStreamIndex;
    int              iAudioStreamIndex;
    int              nRenderMode;
    HWND             hRenderWnd;
    HANDLE           hCoreRender;
    int              nPlayerTime;
    int              nPlayerStatus;
    HANDLE           hPlayerThread;
    CRITICAL_SECTION cs;
} PLAYER;

// 内部函数实现
static DWORD WINAPI PlayThreadProc(PLAYER *player)
{
    AVPacket  packet;
    AVFrame  *aframe;
    AVFrame  *vframe;
    int       gotaudio;
    int       gotvideo;
    int       time;
    int       retv;

    aframe = avcodec_alloc_frame();
    vframe = avcodec_alloc_frame();
    if (!aframe || !vframe) goto exit;

    while (player->nPlayerStatus != PLAYER_STOP)
    {
        EnterCriticalSection(&(player->cs));
        retv = av_read_frame(player->pAVFormatContext, &packet);
        LeaveCriticalSection(&(player->cs));

        // handle stop
        if (retv < 0)
        {
            player->nPlayerStatus = PLAYER_STOP;
            PostMessage(player->hRenderWnd, MSG_COREPLAYER, player->nPlayerStatus, 0);
            goto exit;
        }

        // 播放进度控制
        if (packet.dts > 0) {
            time = (int)(packet.dts
                * player->pAVFormatContext->streams[packet.stream_index]->time_base.num
                / player->pAVFormatContext->streams[packet.stream_index]->time_base.den);
            if (time != player->nPlayerTime) {
                PostMessage(player->hRenderWnd, MSG_COREPLAYER, player->nPlayerStatus, time);
                player->nPlayerTime = time;
            }
        }

        // audio todo..
        if (packet.stream_index == player->iAudioStreamIndex)
        {
            EnterCriticalSection(&(player->cs));
            avcodec_decode_audio(player->pAudioCodecContext, aframe, &gotaudio, &packet);
            LeaveCriticalSection(&(player->cs));

            if (gotaudio) {
                aframe->pts = packet.pts;
                renderaudiowrite(player->hCoreRender, aframe);
                TRACE("apts = %lld\n", aframe->pts);
            }
        }

        // video
        if (packet.stream_index == player->iVideoStreamIndex)
        {
            EnterCriticalSection(&(player->cs));
            avcodec_decode_video(player->pVideoCodecContext, vframe, &gotvideo, &packet);
            LeaveCriticalSection(&(player->cs));

            if (gotvideo) {
                vframe->pts = packet.pts;
                rendervideowrite(player->hCoreRender, vframe);
                TRACE("vpts = %lld\n", vframe->pts);
            }
        }

        // free packet
        av_free_packet(&packet);
    }

exit:
    avcodec_free_frame(&aframe);
    avcodec_free_frame(&vframe);
    return TRUE;
}

// 函数实现
HANDLE playeropen(char *file, HWND hwnd)
{
    PLAYER        *player   = NULL;
    AVCodec       *pAVCodec = NULL;
    int            vformat  = 0;
    int            width    = 0;
    int            height   = 0;
    AVRational     vrate    = {1, 1};
    uint64_t       alayout  = 0;
    int            aformat  = 0;
    int            arate    = 0;
    uint32_t       i        = 0;

    // av register all
    av_register_all();

    // alloc player context
    player = (PLAYER*)malloc(sizeof(PLAYER));
    memset(player, 0, sizeof(PLAYER));

    // init critical section
    InitializeCriticalSection(&(player->cs));

    // open input file
    if (avformat_open_input(&(player->pAVFormatContext), file, NULL, 0) != 0) {
        goto error_handler;
    }

    // find stream info
    if (av_find_stream_info(player->pAVFormatContext) < 0) {
        goto error_handler;
    }

    // get video & audio codec context
    player->iAudioStreamIndex = -1;
    player->iVideoStreamIndex = -1;
    for (i=0; i<player->pAVFormatContext->nb_streams; i++)
    {
        switch (player->pAVFormatContext->streams[i]->codec->codec_type)
        {
        case AVMEDIA_TYPE_AUDIO:
            player->iAudioStreamIndex  = i;
            player->pAudioCodecContext = player->pAVFormatContext->streams[i]->codec;
            break;

        case AVMEDIA_TYPE_VIDEO:
            player->iVideoStreamIndex  = i;
            player->pVideoCodecContext = player->pAVFormatContext->streams[i]->codec;
            vrate = player->pAVFormatContext->streams[i]->r_frame_rate;
            break;
        }
    }

    // open audio codec
    if (player->iAudioStreamIndex != -1)
    {
        pAVCodec = avcodec_find_decoder(player->pAudioCodecContext->codec_id);
        if (pAVCodec)
        {
            if (avcodec_open(player->pAudioCodecContext, pAVCodec) < 0)
            {
                player->iAudioStreamIndex = -1;
            }
        }
        else player->iAudioStreamIndex = -1;
    }

    // open video codec
    if (player->iVideoStreamIndex != -1)
    {
        pAVCodec = avcodec_find_decoder(player->pVideoCodecContext->codec_id);
        if (pAVCodec)
        {
            if (avcodec_open(player->pVideoCodecContext, pAVCodec) < 0)
            {
                player->iVideoStreamIndex = -1;
            }
        }
        else player->iVideoStreamIndex = -1;
    }
    
    // for video
    if (player->iVideoStreamIndex != -1)
    {
        vformat = player->pVideoCodecContext->pix_fmt;
        width   = player->pVideoCodecContext->width;
        height  = player->pVideoCodecContext->height;
    }

    // for audio
    if (player->iAudioStreamIndex != -1)
    {
        alayout = player->pAudioCodecContext->channel_layout;
        aformat = player->pAudioCodecContext->sample_fmt;
        arate   = player->pAudioCodecContext->sample_rate;
    }

    // open core render
    player->hCoreRender = renderopen(hwnd, vrate, vformat, width, height,
        alayout, (AVSampleFormat)aformat, arate);

    // save window handle
    player->hRenderWnd = hwnd;
    return player;

error_handler:
    playerclose((HANDLE)player);
    return NULL;
}

void playerclose(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    // stop player first
    playerstop(hplayer);

    if (player->hCoreRender       ) renderclose(player->hCoreRender);
    if (player->pVideoCodecContext) avcodec_close(player->pVideoCodecContext);
    if (player->pAudioCodecContext) avcodec_close(player->pAudioCodecContext);
    if (player->pAVFormatContext  ) av_close_input_file(player->pAVFormatContext);

    DeleteCriticalSection(&(player->cs));
    free(player);
}

void playerplay(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;
    player->nPlayerStatus = PLAYER_PLAY;
    if (!player->hPlayerThread) {
        player->hPlayerThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)PlayThreadProc, player, 0, 0);
    }
    renderstart(player->hCoreRender);
}

void playerpause(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;
    player->nPlayerStatus = PLAYER_PAUSE;
    renderpause(player->hCoreRender);
}

void playerstop(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    player->nPlayerStatus = PLAYER_STOP;
    renderstart(player->hCoreRender);
    if (player->hPlayerThread)
    {
        WaitForSingleObject(player->hPlayerThread, -1);
        CloseHandle(player->hPlayerThread);
        player->hPlayerThread = NULL;
    }
    renderpause(player->hCoreRender);
    playerseek(player, 0);
}

void playersetrect(HANDLE hplayer, int x, int y, int w, int h)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    int vw, vh;
    int rw, rh;
    playergetparam(hplayer, PARAM_VIDEO_WIDTH , &vw);
    playergetparam(hplayer, PARAM_VIDEO_HEIGHT, &vh);
    if (!vw || !vh) return;

    switch (player->nRenderMode)
    {
    case RENDER_LETTERBOX:
        if (w * vh < h * vw) { rw = w; rh = rw * vh / vw; }
        else                 { rh = h; rw = rh * vw / vh; }
        break;

    case RENDER_STRETCHED:
        rw = w;
        rh = h;
        break;
    }
    rendersetrect(player->hCoreRender, x + (w - rw) / 2, y + (h - rh) / 2, rw, rh);
}

void playerseek(HANDLE hplayer, DWORD sec)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    EnterCriticalSection(&(player->cs));
    av_seek_frame(player->pAVFormatContext, -1, sec * AV_TIME_BASE, 0);
    if (player->iAudioStreamIndex != -1) avcodec_flush_buffers(player->pAudioCodecContext);
    if (player->iVideoStreamIndex != -1) avcodec_flush_buffers(player->pVideoCodecContext);
    LeaveCriticalSection(&(player->cs));
    renderflush(player->hCoreRender);
}

void playersetparam(HANDLE hplayer, DWORD id, DWORD param)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    switch (id)
    {
    case PARAM_RENDER_MODE:
        player->nRenderMode = param;
        break;
    }
}

void playergetparam(HANDLE hplayer, DWORD id, void *param)
{
    if (!hplayer || !param) return;
    PLAYER *player = (PLAYER*)hplayer;

    switch (id)
    {
    case PARAM_VIDEO_WIDTH:
        if (!player->pVideoCodecContext) *(int*)param = 0;
        else *(int*)param = player->pVideoCodecContext->width;
        break;

    case PARAM_VIDEO_HEIGHT:
        if (!player->pVideoCodecContext) *(int*)param = 0;
        else *(int*)param = player->pVideoCodecContext->height;
        break;

    case PARAM_GET_DURATION:
        if (!player->pAVFormatContext) *(DWORD*)param = 0;
        else *(DWORD*)param = (DWORD)(player->pAVFormatContext->duration / AV_TIME_BASE);
        break;

    case PARAM_RENDER_MODE:
        *(int*)param = player->nRenderMode;
        break;
    }
}





