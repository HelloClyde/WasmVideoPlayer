#include <stdio.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <unistd.h>
#include <float.h>
#include <errno.h>

typedef void(*VideoCallback)(unsigned char *buff, int size, double timestamp);
typedef void(*AudioCallback)(unsigned char *buff, int size, double timestamp);
typedef void(*RequestCallback)(int track, int offset, int available);

#ifdef __cplusplus
extern "C" {
#endif

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/fifo.h"
//#include "libswscale/swscale.h"

#define MIN(X, Y)  ((X) < (Y) ? (X) : (Y))

const int kCustomIoBufferSize = 32 * 1024;
const int kInitialPcmBufferSize = 128 * 1024;
const int kDefaultFifoSize = 1 * 1024 * 1024;
const int kMaxFifoSize = 16 * 1024 * 1024;
const int kSeparateStreamDisabled = -2;

typedef enum ErrorCode {
    kErrorCode_Success = 0,
    kErrorCode_Invalid_Param,
    kErrorCode_Invalid_State,
    kErrorCode_Invalid_Data,
    kErrorCode_Invalid_Format,
    kErrorCode_NULL_Pointer,
    kErrorCode_Open_File_Error,
    kErrorCode_Eof,
    kErrorCode_FFmpeg_Error,
    kErrorCode_Old_Frame
} ErrorCode;

typedef enum LogLevel {
    kLogLevel_None, //Not logging.
    kLogLevel_Core, //Only logging core module(without ffmpeg).
    kLogLevel_All   //Logging all, with ffmpeg.
} LogLevel;

#define TRACK_TYPE_VIDEO 0
#define TRACK_TYPE_AUDIO 1

typedef struct DecoderStream {
    enum AVMediaType type;
    AVFormatContext *formatContext;
    AVCodecContext *codecContext;
    AVIOContext *ioContext;
    int streamIdx;
    AVRational timeBase;
    unsigned char *frameBuffer;
    int bufferCapacity;
    int frameSize;
    unsigned char *customIoBuffer;

    FILE *fp;
    char fileName[64];
    int64_t fileSize;
    int64_t fileReadPos;
    int64_t fileWritePos;
    int64_t lastRequestOffset;
    int isStream;
    AVFifoBuffer *fifo;
    int fifoSize;

    int trackType;
    int opened;
    int reachedEof;
    double lastPts;
    int hasDecodedFrame;
} DecoderStream;

typedef struct WebDecoder {
    AVFormatContext *avformatContext;
    int multiStream;
    DecoderStream videoStream;
    DecoderStream audioStream;
    AVFrame *avFrame;
    VideoCallback videoCallback;
    AudioCallback audioCallback;
    RequestCallback requestCallback;
    //struct SwsContext* swsCtx;
    unsigned char *customIoBuffer;
    FILE *fp;
    char fileName[64];
    int64_t fileSize;
    int64_t fileReadPos;
    int64_t fileWritePos;
    int64_t lastRequestOffset;
    double beginTimeOffset;
    int accurateSeek;
    // For streaming.
    int isStream;
    AVFifoBuffer *fifo;
    int fifoSize;
} WebDecoder;

WebDecoder *decoder = NULL;
LogLevel logLevel = kLogLevel_None;

int getAailableDataSize();
int streamRead(DecoderStream *stream, uint8_t *data, int len);
int streamWrite(DecoderStream *stream, unsigned char *buff, int size);
int streamAvailableDataSize(DecoderStream *stream);
int64_t streamSeek(DecoderStream *stream, int64_t offset, int whence);
ErrorCode initStreamBuffer(DecoderStream *stream, int64_t fileSize);
ErrorCode openStreamDecoder(DecoderStream *stream);

void initDecoderStream(DecoderStream *stream, enum AVMediaType type) {
    if (stream == NULL) {
        return;
    }

    stream->type = type;
    stream->formatContext = NULL;
    stream->codecContext = NULL;
    stream->ioContext = NULL;
    stream->streamIdx = -1;
    stream->timeBase.num = 0;
    stream->timeBase.den = 1;
    stream->frameBuffer = NULL;
    stream->bufferCapacity = 0;
    stream->frameSize = 0;
    stream->customIoBuffer = NULL;
    stream->fp = NULL;
    stream->fileName[0] = '\0';
    stream->fileSize = 0;
    stream->fileReadPos = 0;
    stream->fileWritePos = 0;
    stream->lastRequestOffset = 0;
    stream->isStream = 0;
    stream->fifo = NULL;
    stream->fifoSize = 0;
    stream->trackType = (type == AVMEDIA_TYPE_VIDEO) ? TRACK_TYPE_VIDEO : TRACK_TYPE_AUDIO;
    stream->opened = 0;
    stream->reachedEof = 0;
    stream->lastPts = 0.0;
    stream->hasDecodedFrame = 0;
}

void releaseDecoderStream(DecoderStream *stream) {
    if (stream == NULL) {
        return;
    }

    if (stream->frameBuffer != NULL) {
        av_freep(&stream->frameBuffer);
    }

    if (stream->ioContext != NULL) {
        avio_context_free(&stream->ioContext);
        stream->customIoBuffer = NULL;
    }

    if (stream->formatContext != NULL) {
        avformat_close_input(&stream->formatContext);
    }

    if (stream->customIoBuffer != NULL) {
        av_freep(&stream->customIoBuffer);
    }

    if (stream->fifo != NULL) {
        av_fifo_freep(&stream->fifo);
        stream->fifoSize = 0;
    }

    if (stream->fp != NULL) {
        fclose(stream->fp);
        stream->fp = NULL;
        if (stream->fileName[0] != '\0') {
            remove(stream->fileName);
            stream->fileName[0] = '\0';
        }
    }

    stream->bufferCapacity = 0;
    stream->frameSize = 0;
    stream->codecContext = NULL;
    stream->streamIdx = -1;
    stream->timeBase.num = 0;
    stream->timeBase.den = 1;
    stream->fileSize = 0;
    stream->fileReadPos = 0;
    stream->fileWritePos = 0;
    stream->lastRequestOffset = 0;
    stream->isStream = 0;
    stream->opened = 0;
    stream->reachedEof = 0;
    stream->lastPts = 0.0;
    stream->hasDecodedFrame = 0;
}

unsigned long getTickCount() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * (unsigned long)1000 + ts.tv_nsec / 1000000;
}

void simpleLog(const char* format, ...) {
    if (logLevel == kLogLevel_None) {
        return;
    }

    char szBuffer[1024] = { 0 };
    char szTime[32]		= { 0 };
    char *p				= NULL;
    int prefixLength	= 0;
    const char *tag		= "Core";
    struct tm tmTime;
    struct timeb tb;

    ftime(&tb);
    localtime_r(&tb.time, &tmTime);

    if (1) {
        int tmYear		= tmTime.tm_year + 1900;
        int tmMon		= tmTime.tm_mon + 1;
        int tmMday		= tmTime.tm_mday;
        int tmHour		= tmTime.tm_hour;
        int tmMin		= tmTime.tm_min;
        int tmSec		= tmTime.tm_sec;
        int tmMillisec	= tb.millitm;
        sprintf(szTime, "%d-%d-%d %d:%d:%d.%d", tmYear, tmMon, tmMday, tmHour, tmMin, tmSec, tmMillisec);
    }

    prefixLength = sprintf(szBuffer, "[%s][%s][DT] ", szTime, tag);
    p = szBuffer + prefixLength;
    
    if (1) {
        va_list ap;
        va_start(ap, format);
        vsnprintf(p, 1024 - prefixLength, format, ap);
        va_end(ap);
    }

    printf("%s\n", szBuffer);
}

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
    static int printPrefix	= 1;
    static int count		= 0;
    static char prev[1024]	= { 0 };
    char line[1024]			= { 0 };
    static int is_atty;
    AVClass* avc = ptr ? *(AVClass**)ptr : NULL;
    if (level > AV_LOG_DEBUG) {
        return;
    }

    line[0] = 0;

    if (printPrefix && avc) {
        if (avc->parent_log_context_offset) {
            AVClass** parent = *(AVClass***)(((uint8_t*)ptr) + avc->parent_log_context_offset);
            if (parent && *parent) {
                snprintf(line, sizeof(line), "[%s @ %p] ", (*parent)->item_name(parent), parent);
            }
        }
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "[%s @ %p] ", avc->item_name(ptr), ptr);
    }

    vsnprintf(line + strlen(line), sizeof(line) - strlen(line), fmt, vl);
    line[strlen(line) + 1] = 0;
    simpleLog("%s", line);
}

int openCodecContext(AVFormatContext *fmtCtx, enum AVMediaType type, int *streamIdx, AVCodecContext **decCtx) {
    int ret = 0;
    do {
        int streamIndex		= -1;
        AVStream *st		= NULL;
        AVCodec *dec		= NULL;
        AVDictionary *opts	= NULL;

        ret = av_find_best_stream(fmtCtx, type, -1, -1, NULL, 0);
        if (ret < 0) {
            simpleLog("Could not find %s stream.", av_get_media_type_string(type));
            break;
        }

        streamIndex = ret;
        st = fmtCtx->streams[streamIndex];

        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            simpleLog("Failed to find %s codec %d.", av_get_media_type_string(type), st->codecpar->codec_id);
            ret = AVERROR(EINVAL);
            break;
        }

        *decCtx = avcodec_alloc_context3(dec);
        if (!*decCtx) {
            simpleLog("Failed to allocate the %s codec context.", av_get_media_type_string(type));
            ret = AVERROR(ENOMEM);
            break;
        }

        if ((ret = avcodec_parameters_to_context(*decCtx, st->codecpar)) != 0) {
            simpleLog("Failed to copy %s codec parameters to decoder context.", av_get_media_type_string(type));
            break;
        }

        av_dict_set(&opts, "refcounted_frames", "0", 0);

        if ((ret = avcodec_open2(*decCtx, dec, NULL)) != 0) {
            simpleLog("Failed to open %s codec.", av_get_media_type_string(type));
            break;
        }

        *streamIdx = streamIndex;
        avcodec_flush_buffers(*decCtx);
    } while (0);

    return ret;
}

void closeCodecContext(AVFormatContext *fmtCtx, AVCodecContext *decCtx, int streamIdx) {
    do {
        if (fmtCtx == NULL || decCtx == NULL) {
            break;
        }

        if (streamIdx < 0 || streamIdx >= fmtCtx->nb_streams) {
            break;
        }

        fmtCtx->streams[streamIdx]->discard = AVDISCARD_ALL;
        avcodec_close(decCtx);
    } while (0);
}

ErrorCode copyYuvData(AVFrame *frame, unsigned char *buffer, int width, int height) {
    ErrorCode ret		= kErrorCode_Success;
    unsigned char *src	= NULL;
    unsigned char *dst	= buffer;
    int i = 0;
    do {
        if (frame == NULL || buffer == NULL) {
            ret = kErrorCode_Invalid_Param;
            break;
        }

        if (!frame->data[0] || !frame->data[1] || !frame->data[2]) {
            ret = kErrorCode_Invalid_Param;
            break;
        }

        for (i = 0; i < height; i++) {
            src = frame->data[0] + i * frame->linesize[0];
            memcpy(dst, src, width);
            dst += width;
        }

        for (i = 0; i < height / 2; i++) {
            src = frame->data[1] + i * frame->linesize[1];
            memcpy(dst, src, width / 2);
            dst += width / 2;
        }

        for (i = 0; i < height / 2; i++) {
            src = frame->data[2] + i * frame->linesize[2];
            memcpy(dst, src, width / 2);
            dst += width / 2;
        }
    } while (0);
    return ret;	
}

/*
ErrorCode yuv420pToRgb32(unsigned char *yuvBuff, unsigned char *rgbBuff, int width, int height) {
    ErrorCode ret = kErrorCode_Success;
    AVPicture yuvPicture, rgbPicture;
    uint8_t *ptmp = NULL;
    do {
        if (yuvBuff == NULL || rgbBuff == NULL) {
            ret = kErrorCode_Invalid_Param
            break;
        }

        if (decoder == NULL || decoder->swsCtx == NULL) {
            ret = kErrorCode_Invalid_Param
            break;
        }

        
        avpicture_fill(&yuvPicture, yuvBuff, AV_PIX_FMT_YUV420P, width, height);
        avpicture_fill(&rgbPicture, rgbBuff, AV_PIX_FMT_RGB32, width, height);

        ptmp = yuvPicture.data[1];
        yuvPicture.data[1] = yuvPicture.data[2];
        yuvPicture.data[2] = ptmp;

        sws_scale(decoder->swsCtx, yuvPicture.data, yuvPicture.linesize, 0, height, rgbPicture.data, rgbPicture.linesize);
    } while (0);
    return ret;
}
*/

int roundUp(int numToRound, int multiple) {
    return (numToRound + multiple - 1) & -multiple;
}

ErrorCode processDecodedVideoFrame(AVFrame *frame) {
    ErrorCode ret = kErrorCode_Success;
    double timestamp = 0.0f;
    DecoderStream *video = NULL;
    do {
        if (decoder == NULL) {
            ret = kErrorCode_Invalid_State;
            break;
        }

        video = &decoder->videoStream;

        if (frame == NULL ||
            decoder->videoCallback == NULL ||
            video->frameBuffer == NULL ||
            video->bufferCapacity <= 0 ||
            video->codecContext == NULL) {
            ret = kErrorCode_Invalid_Param;
            break;
        }

        if (video->codecContext->pix_fmt != AV_PIX_FMT_YUV420P) {
            simpleLog("Not YUV420P, but unsupported format %d.", video->codecContext->pix_fmt);
            ret = kErrorCode_Invalid_Format;
            break;
        }

        ret = copyYuvData(frame, video->frameBuffer, video->codecContext->width, video->codecContext->height);
        if (ret != kErrorCode_Success) {
            break;
        }

        /*
        ret = yuv420pToRgb32(video->frameBuffer, decoder->rgbBuffer, decoder->videoStream.codecContext->width, decoder->videoStream.codecContext->height);
        if (ret != kErrorCode_Success) {
            break;
        }
        */

        int64_t ptsValue = frame->pts;
        if (ptsValue == AV_NOPTS_VALUE) {
            ptsValue = frame->best_effort_timestamp;
        }

        AVRational timeBase;
        if (decoder->multiStream) {
            timeBase = video->timeBase;
        } else {
            timeBase = decoder->avformatContext->streams[video->streamIdx]->time_base;
        }

        timestamp = (double)ptsValue * av_q2d(timeBase);

        if (decoder->accurateSeek && timestamp < decoder->beginTimeOffset) {
            //simpleLog("video timestamp %lf < %lf", timestamp, decoder->beginTimeOffset);
            ret = kErrorCode_Old_Frame;
            break;
        }
        decoder->videoCallback(video->frameBuffer, video->frameSize, timestamp);
        video->lastPts = timestamp;
        video->hasDecodedFrame = 1;
    } while (0);
    return ret;
}

ErrorCode processDecodedAudioFrame(AVFrame *frame) {
    ErrorCode ret       = kErrorCode_Success;
    int sampleSize      = 0;
    int audioDataSize   = 0;
    int targetSize      = 0;
    int offset          = 0;
    int i               = 0;
    int ch              = 0;
    double timestamp    = 0.0f;
    DecoderStream *audio = NULL;
    do {
        if (decoder == NULL) {
            ret = kErrorCode_Invalid_State;
            break;
        }

        audio = &decoder->audioStream;

        if (frame == NULL || audio->codecContext == NULL) {
            ret = kErrorCode_Invalid_Param;
            break;
        }

        sampleSize = av_get_bytes_per_sample(audio->codecContext->sample_fmt);
        if (sampleSize < 0) {
            simpleLog("Failed to calculate data size.");
            ret = kErrorCode_Invalid_Data;
            break;
        }

        if (audio->frameBuffer == NULL) {
            audio->frameBuffer = (unsigned char*)av_mallocz(kInitialPcmBufferSize);
            audio->bufferCapacity = kInitialPcmBufferSize;
            simpleLog("Initial PCM buffer size %d.", audio->bufferCapacity);
        }

        audioDataSize = frame->nb_samples * audio->codecContext->channels * sampleSize;
        if (audio->bufferCapacity < audioDataSize) {
            targetSize = roundUp(audioDataSize, 4);
            simpleLog("Current PCM buffer size %d not sufficient for data size %d, round up to target %d.",
                audio->bufferCapacity,
                audioDataSize,
                targetSize);
            audio->bufferCapacity = targetSize;
            av_free(audio->frameBuffer);
            audio->frameBuffer = (unsigned char*)av_mallocz(audio->bufferCapacity);
        }

        for (i = 0; i < frame->nb_samples; i++) {
            for (ch = 0; ch < audio->codecContext->channels; ch++) {
                memcpy(audio->frameBuffer + offset, frame->data[ch] + sampleSize * i, sampleSize);
                offset += sampleSize;
            }
        }

        int64_t ptsValue = frame->pts;
        if (ptsValue == AV_NOPTS_VALUE) {
            ptsValue = frame->best_effort_timestamp;
        }

        AVRational timeBase;
        if (decoder->multiStream) {
            timeBase = audio->timeBase;
        } else {
            timeBase = decoder->avformatContext->streams[audio->streamIdx]->time_base;
        }

        timestamp = (double)ptsValue * av_q2d(timeBase);

        if (decoder->accurateSeek && timestamp < decoder->beginTimeOffset) {
            //simpleLog("audio timestamp %lf < %lf", timestamp, decoder->beginTimeOffset);
            ret = kErrorCode_Old_Frame;
            break;
        }
        audio->frameSize = audioDataSize;
        if (decoder->audioCallback != NULL) {
            decoder->audioCallback(audio->frameBuffer, audioDataSize, timestamp);
        }
        audio->lastPts = timestamp;
        audio->hasDecodedFrame = 1;
    } while (0);
    return ret;
}

ErrorCode decodePacket(DecoderStream *stream, AVPacket *pkt, int *decodedLen) {
    int ret = 0;
    int isVideo = 0;
    AVCodecContext *codecContext = NULL;

    if (stream == NULL || pkt == NULL || decodedLen == NULL) {
        simpleLog("decodePacket invalid param.");
        return kErrorCode_Invalid_Param;
    }

    *decodedLen = 0;

    codecContext = stream->codecContext;
    if (codecContext == NULL) {
        return kErrorCode_Invalid_State;
    }

    isVideo = (stream->type == AVMEDIA_TYPE_VIDEO);

    ret = avcodec_send_packet(codecContext, pkt);
    if (ret < 0) {
        simpleLog("Error sending a packet for decoding %d.", ret);
        return kErrorCode_FFmpeg_Error;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codecContext, decoder->avFrame);
        if (ret == AVERROR(EAGAIN)) {
            return kErrorCode_Success;
        } else if (ret == AVERROR_EOF) {
            return kErrorCode_Eof;
        } else if (ret < 0) {
            simpleLog("Error during decoding %d.", ret);
            return kErrorCode_FFmpeg_Error;
        } else {
            int r = isVideo ? processDecodedVideoFrame(decoder->avFrame) : processDecodedAudioFrame(decoder->avFrame);
            if (r == kErrorCode_Old_Frame) {
                return r;
            }
        }
    }

    *decodedLen = pkt->size;
    return kErrorCode_Success;
}

DecoderStream* selectNextStreamForDecode() {
    DecoderStream *video = &decoder->videoStream;
    DecoderStream *audio = &decoder->audioStream;

    if (video->reachedEof && audio->reachedEof) {
        return NULL;
    }

    if (!video->reachedEof && !video->hasDecodedFrame) {
        return video;
    }

    if (!audio->reachedEof && !audio->hasDecodedFrame) {
        return audio;
    }

    if (video->reachedEof) {
        return audio;
    }

    if (audio->reachedEof) {
        return video;
    }

    return (video->lastPts <= audio->lastPts) ? video : audio;
}

ErrorCode decodeStreamPacket(DecoderStream *stream) {
    if (stream == NULL || stream->formatContext == NULL) {
        return kErrorCode_Invalid_State;
    }

    ErrorCode ret = kErrorCode_Success;
    int decodedLen = 0;
    int r = 0;
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    r = av_read_frame(stream->formatContext, &packet);
    if (r == AVERROR_EOF) {
        stream->reachedEof = 1;
        ret = kErrorCode_Eof;
        goto end;
    }

    if (r == AVERROR(EAGAIN)) {
        ret = kErrorCode_Invalid_State;
        goto end;
    }

    if (r < 0 || packet.size == 0) {
        ret = kErrorCode_FFmpeg_Error;
        goto end;
    }

    do {
        ret = decodePacket(stream, &packet, &decodedLen);
        if (ret != kErrorCode_Success) {
            break;
        }

        if (decodedLen <= 0) {
            break;
        }

        packet.data += decodedLen;
        packet.size -= decodedLen;
    } while (packet.size > 0);

end:
    av_packet_unref(&packet);
    return ret;
}

int readFromFile(uint8_t *data, int len) {
    //simpleLog("readFromFile %d.", len);
    int32_t ret         = -1;
    int availableBytes  = 0;
    int canReadLen      = 0;
    do {
        if (decoder->fp == NULL) {
            break;
        }

        availableBytes = decoder->fileWritePos - decoder->fileReadPos;
        if (availableBytes <= 0) {
            break;
        }

        fseek(decoder->fp, decoder->fileReadPos, SEEK_SET);
        canReadLen = MIN(availableBytes, len);
        fread(data, canReadLen, 1, decoder->fp);
        decoder->fileReadPos += canReadLen;
        ret = canReadLen;
    } while (0);
    //simpleLog("readFromFile ret %d.", ret);
    return ret;
}

int readFromFifo(uint8_t *data, int len) {
    //simpleLog("readFromFifo %d.", len);
    int32_t ret         = -1;
    int availableBytes  = 0;
    int canReadLen      = 0;
    do {
        if (decoder->fifo == NULL) {
            break;
        }	

        availableBytes = av_fifo_size(decoder->fifo);
        if (availableBytes <= 0) {
            break;
        }

        canReadLen = MIN(availableBytes, len);
        av_fifo_generic_read(decoder->fifo, data, canReadLen, NULL);
        ret = canReadLen;
    } while (0);
    //simpleLog("readFromFifo ret %d, left %d.", ret, av_fifo_size(decoder->fifo));
    return ret;
}

int streamRead(DecoderStream *stream, uint8_t *data, int len) {
    if (stream == NULL || data == NULL || len <= 0) {
        return -1;
    }

    if (stream->isStream) {
        if (stream->fifo == NULL) {
            return -1;
        }

        int availableBytes = av_fifo_size(stream->fifo);
        if (availableBytes <= 0) {
            return -1;
        }

        int canReadLen = MIN(availableBytes, len);
        av_fifo_generic_read(stream->fifo, data, canReadLen, NULL);
        return canReadLen;
    }

    if (stream->fp == NULL) {
        return -1;
    }

    int64_t availableBytes = stream->fileWritePos - stream->fileReadPos;
    if (availableBytes <= 0) {
        return -1;
    }

    int canReadLen = MIN(availableBytes, len);
    fseek(stream->fp, stream->fileReadPos, SEEK_SET);
    fread(data, canReadLen, 1, stream->fp);
    stream->fileReadPos += canReadLen;
    return canReadLen;
}

int streamWrite(DecoderStream *stream, unsigned char *buff, int size) {
    if (stream == NULL || buff == NULL || size <= 0) {
        return -1;
    }

    if (stream->isStream) {
        if (stream->fifo == NULL) {
            return -1;
        }

        int64_t leftSpace = av_fifo_space(stream->fifo);
        if (leftSpace < size) {
            int growSize = 0;
            do {
                leftSpace += stream->fifoSize;
                growSize += stream->fifoSize;
                stream->fifoSize += stream->fifoSize;
            } while (leftSpace < size);
            av_fifo_grow(stream->fifo, growSize);

            simpleLog("Fifo size growed to %d for track %d.", stream->fifoSize, stream->trackType);
            if (stream->fifoSize >= kMaxFifoSize) {
                simpleLog("[Warn] Fifo size larger than %d.", kMaxFifoSize);
            }
        }

        return av_fifo_generic_write(stream->fifo, buff, size, NULL);
    }

    if (stream->fp == NULL) {
        return -1;
    }

    int64_t leftBytes = stream->fileSize - stream->fileWritePos;
    if (leftBytes <= 0) {
        return 0;
    }

    int canWriteBytes = MIN(leftBytes, size);
    fseek(stream->fp, stream->fileWritePos, SEEK_SET);
    fwrite(buff, canWriteBytes, 1, stream->fp);
    stream->fileWritePos += canWriteBytes;
    return canWriteBytes;
}

int streamAvailableDataSize(DecoderStream *stream) {
    if (stream == NULL) {
        return 0;
    }

    if (stream->isStream) {
        return stream->fifo == NULL ? 0 : av_fifo_size(stream->fifo);
    }

    return stream->fileWritePos - stream->fileReadPos;
}

int64_t streamSeek(DecoderStream *stream, int64_t offset, int whence) {
    if (stream == NULL || stream->isStream || stream->fp == NULL) {
        return -1;
    }

    if (whence == AVSEEK_SIZE) {
        return stream->fileSize;
    }

    if (whence != SEEK_END && whence != SEEK_SET && whence != SEEK_CUR) {
        return -1;
    }

    int ret = fseek(stream->fp, (long)offset, whence);
    if (ret == -1) {
        return -1;
    }

    int64_t pos = (int64_t)ftell(stream->fp);
    if (pos < stream->lastRequestOffset || pos > stream->fileWritePos) {
        stream->lastRequestOffset = pos;
        stream->fileReadPos = pos;
        stream->fileWritePos = pos;
        if (decoder != NULL && decoder->requestCallback != NULL) {
            decoder->requestCallback(stream->trackType, (int)pos, streamAvailableDataSize(stream));
        }
        return -1;
    }

    stream->fileReadPos = pos;
    return pos;
}

ErrorCode initStreamBuffer(DecoderStream *stream, int64_t fileSize) {
    if (stream == NULL) {
        return kErrorCode_Invalid_Param;
    }

    if (fileSize >= 0) {
        stream->fileSize = fileSize;
        sprintf(stream->fileName, "tmp-%s-%lu.dat",
            stream->trackType == TRACK_TYPE_VIDEO ? "video" : "audio",
            getTickCount());
        stream->fp = fopen(stream->fileName, "wb+");
        if (stream->fp == NULL) {
            simpleLog("Open file %s failed, err: %d.", stream->fileName, errno);
            return kErrorCode_Open_File_Error;
        }
    } else {
        stream->isStream = 1;
        stream->fifoSize = kDefaultFifoSize;
        stream->fifo = av_fifo_alloc(stream->fifoSize);
        if (stream->fifo == NULL) {
            return kErrorCode_Invalid_State;
        }
    }

    return kErrorCode_Success;
}

ErrorCode openStreamDecoder(DecoderStream *stream) {
    if (stream == NULL) {
        return kErrorCode_Invalid_Param;
    }

    ErrorCode ret = kErrorCode_Success;
    int r = 0;

    stream->formatContext = avformat_alloc_context();
    if (stream->formatContext == NULL) {
        return kErrorCode_Invalid_State;
    }

    stream->customIoBuffer = (unsigned char*)av_mallocz(kCustomIoBufferSize);
    if (stream->customIoBuffer == NULL) {
        ret = kErrorCode_Invalid_State;
        goto fail;
    }

    stream->ioContext = avio_alloc_context(
        stream->customIoBuffer,
        kCustomIoBufferSize,
        0,
        stream,
        readCallback,
        NULL,
        stream->isStream ? NULL : seekCallback);
    if (stream->ioContext == NULL) {
        simpleLog("avio_alloc_context failed for track %d.", stream->trackType);
        ret = kErrorCode_FFmpeg_Error;
        goto fail;
    }

    stream->formatContext->pb = stream->ioContext;
    stream->formatContext->flags = AVFMT_FLAG_CUSTOM_IO;

    r = avformat_open_input(&stream->formatContext, NULL, NULL, NULL);
    if (r != 0) {
        char err_info[32] = { 0 };
        av_strerror(r, err_info, 32);
        simpleLog("avformat_open_input failed for track %d %d %s.", stream->trackType, r, err_info);
        ret = kErrorCode_FFmpeg_Error;
        goto fail;
    }

    r = avformat_find_stream_info(stream->formatContext, NULL);
    if (r != 0) {
        simpleLog("avformat_find_stream_info failed for track %d %d.", stream->trackType, r);
        ret = kErrorCode_FFmpeg_Error;
        goto fail;
    }

    r = openCodecContext(
        stream->formatContext,
        stream->type,
        &stream->streamIdx,
        &stream->codecContext);
    if (r != 0) {
        simpleLog("Open codec context failed for track %d %d.", stream->trackType, r);
        ret = kErrorCode_FFmpeg_Error;
        goto fail;
    }

    stream->timeBase = stream->formatContext->streams[stream->streamIdx]->time_base;
    stream->opened = 1;
    stream->reachedEof = 0;
    stream->hasDecodedFrame = 0;
    stream->lastPts = 0.0;

    avcodec_flush_buffers(stream->codecContext);

    return ret;

fail:
    if (stream->codecContext != NULL) {
        avcodec_close(stream->codecContext);
        avcodec_free_context(&stream->codecContext);
        stream->codecContext = NULL;
    }
    stream->streamIdx = -1;

    if (stream->ioContext != NULL) {
        avio_context_free(&stream->ioContext);
        stream->customIoBuffer = NULL;
    }

    if (stream->customIoBuffer != NULL) {
        av_freep(&stream->customIoBuffer);
    }

    if (stream->formatContext != NULL) {
        if (stream->formatContext->iformat != NULL) {
            avformat_close_input(&stream->formatContext);
        } else {
            avformat_free_context(stream->formatContext);
            stream->formatContext = NULL;
        }
    }

    stream->opened = 0;
    stream->reachedEof = 0;
    stream->hasDecodedFrame = 0;
    stream->lastPts = 0.0;

    return ret;
}

int readCallback(void *opaque, uint8_t *data, int len) {
    //simpleLog("readCallback %d.", len);
    int32_t ret         = -1;
    do {
        if (decoder == NULL) {
            break;
        }

        if (data == NULL || len <= 0) {
            break;
        }

        DecoderStream *stream = (DecoderStream *)opaque;
        if (stream != NULL) {
            ret = streamRead(stream, data, len);
            if (ret < 0 && decoder->requestCallback != NULL) {
                decoder->requestCallback(stream->trackType, -1, streamAvailableDataSize(stream));
            }
        } else {
            ret = decoder->isStream ? readFromFifo(data, len) : readFromFile(data, len);
        }
    } while (0);
    //simpleLog("readCallback ret %d.", ret);
    return ret;
}

int64_t seekCallback(void *opaque, int64_t offset, int whence) {
    int64_t ret         = -1;
    int64_t pos         = -1;
    int64_t req_pos     = -1;
    //simpleLog("seekCallback %lld %d.", offset, whence);
    do {
        DecoderStream *stream = (DecoderStream *)opaque;
        if (stream != NULL) {
            ret = streamSeek(stream, offset, whence);
            break;
        }

        if (decoder == NULL || decoder->isStream || decoder->fp == NULL) {
            break;
        }

        if (whence == AVSEEK_SIZE) {
            ret = decoder->fileSize;
            break;
        }

        if (whence != SEEK_END && whence != SEEK_SET && whence != SEEK_CUR) {
            break;
        }

        ret = fseek(decoder->fp, (long)offset, whence);
        if (ret == -1) {
            break;
        }

        pos = (int64_t)ftell(decoder->fp);
        if (pos < decoder->lastRequestOffset || pos > decoder->fileWritePos) {
            decoder->lastRequestOffset  = pos;
            decoder->fileReadPos        = pos;
            decoder->fileWritePos       = pos;
            req_pos                     = pos;
            ret                         = -1;  // Forcing not to call read at once.
            decoder->requestCallback(pos, getAailableDataSize());
            simpleLog("Will request %lld and return %lld.", pos, ret);
            break;
        }

        decoder->fileReadPos = pos;
        ret = pos;
    } while (0);
    //simpleLog("seekCallback return %lld.", ret);

    if (decoder != NULL && decoder->requestCallback != NULL) {
        DecoderStream *stream = (DecoderStream *)opaque;
        if (stream != NULL) {
            decoder->requestCallback(stream->trackType, (int)req_pos, streamAvailableDataSize(stream));
        } else {
            decoder->requestCallback(TRACK_TYPE_VIDEO, req_pos, getAailableDataSize());
        }
    }
    return ret;
}

int writeToFile(unsigned char *buff, int size) {
    int ret = 0;
    int64_t leftBytes = 0;
    int canWriteBytes = 0;
    do {
        if (decoder->fp == NULL) {
            ret = -1;
            break;
        }

        leftBytes = decoder->fileSize - decoder->fileWritePos;
        if (leftBytes <= 0) {
            break;
        }

        canWriteBytes = MIN(leftBytes, size);
        fseek(decoder->fp, decoder->fileWritePos, SEEK_SET);
        fwrite(buff, canWriteBytes, 1, decoder->fp);
        decoder->fileWritePos += canWriteBytes;
        ret = canWriteBytes;
    } while (0);
    return ret;
}

int writeToFifo(unsigned char *buff, int size) {
    int ret = 0;
    do {
        if (decoder->fifo == NULL) {
            ret = -1;
            break;
        }

        int64_t leftSpace = av_fifo_space(decoder->fifo);
        if (leftSpace < size) {
            int growSize = 0;
            do {
                leftSpace += decoder->fifoSize;
                growSize += decoder->fifoSize;
                decoder->fifoSize += decoder->fifoSize;
            } while (leftSpace < size);
            av_fifo_grow(decoder->fifo, growSize);

            simpleLog("Fifo size growed to %d.", decoder->fifoSize);
            if (decoder->fifoSize >= kMaxFifoSize) {
                simpleLog("[Warn] Fifo size larger than %d.", kMaxFifoSize);
            }
        }

        //simpleLog("Wrote %d bytes to fifo, total %d.", size, av_fifo_size(decoder->fifo));
        ret = av_fifo_generic_write(decoder->fifo, buff, size, NULL);
    } while (0);
    return ret;
}

int getAailableDataSize() {
    int ret = 0;
    do {
        if (decoder == NULL) {
            break;
        }

        if (decoder->multiStream) {
            ret = streamAvailableDataSize(&decoder->videoStream) +
                  streamAvailableDataSize(&decoder->audioStream);
        } else if (decoder->isStream) {
            ret = decoder->fifo == NULL ? 0 : av_fifo_size(decoder->fifo);
        } else {
            ret = decoder->fileWritePos - decoder->fileReadPos;
        }
    } while (0);
    return ret;
}

//////////////////////////////////Export methods////////////////////////////////////////
ErrorCode initDecoder(int videoFileSize, int audioFileSize, int logLv) {
    ErrorCode ret = kErrorCode_Success;
    do {
        //Log level.
        logLevel = logLv;

        if (decoder != NULL) {
            break;
        }

        decoder = (WebDecoder *)av_mallocz(sizeof(WebDecoder));
        if (decoder == NULL) {
            ret = kErrorCode_Invalid_State;
            break;
        }

        initDecoderStream(&decoder->videoStream, AVMEDIA_TYPE_VIDEO);
        initDecoderStream(&decoder->audioStream, AVMEDIA_TYPE_AUDIO);

        decoder->multiStream = (audioFileSize != kSeparateStreamDisabled);

        if (decoder->multiStream) {
            ret = initStreamBuffer(&decoder->videoStream, videoFileSize);
            if (ret != kErrorCode_Success) {
                break;
            }

            ret = initStreamBuffer(&decoder->audioStream, audioFileSize);
            if (ret != kErrorCode_Success) {
                break;
            }
        } else {
            if (videoFileSize >= 0) {
                decoder->fileSize = videoFileSize;
                sprintf(decoder->fileName, "tmp-%lu.mp4", getTickCount());
                decoder->fp = fopen(decoder->fileName, "wb+");
                if (decoder->fp == NULL) {
                    simpleLog("Open file %s failed, err: %d.", decoder->fileName, errno);
                    ret = kErrorCode_Open_File_Error;
                    break;
                }
            } else {
                decoder->isStream = 1;
                decoder->fifoSize = kDefaultFifoSize;
                decoder->fifo = av_fifo_alloc(decoder->fifoSize);
                if (decoder->fifo == NULL) {
                    ret = kErrorCode_Invalid_State;
                    break;
                }
            }
        }
    } while (0);
    if (ret != kErrorCode_Success) {
        if (decoder != NULL) {
            if (!decoder->multiStream) {
                if (decoder->fp != NULL) {
                    fclose(decoder->fp);
                    decoder->fp = NULL;
                    remove(decoder->fileName);
                }
                if (decoder->fifo != NULL) {
                    av_fifo_freep(&decoder->fifo);
                }
            }
            releaseDecoderStream(&decoder->videoStream);
            releaseDecoderStream(&decoder->audioStream);
            av_freep(&decoder);
        }
    }
    simpleLog("Decoder initialized %d.", ret);
    return ret;
}

ErrorCode uninitDecoder() {
    if (decoder != NULL) {
        if (decoder->fp != NULL) {
            fclose(decoder->fp);
            decoder->fp = NULL;
            remove(decoder->fileName);
        }

        if (decoder->fifo != NULL) {
             av_fifo_freep(&decoder->fifo);
        }

        releaseDecoderStream(&decoder->videoStream);
        releaseDecoderStream(&decoder->audioStream);

        av_freep(&decoder);
    }

    av_log_set_callback(NULL);

    simpleLog("Decoder uninitialized.");
    return kErrorCode_Success;
}

ErrorCode openDecoder(int *paramArray, int paramCount, long videoCallback, long audioCallback, long requestCallback) {
    ErrorCode ret = kErrorCode_Success;
    int r = 0;
    int i = 0;
    int params[7] = { 0 };
    do {
        simpleLog("Opening decoder.");

        av_register_all();
        avcodec_register_all();

        if (logLevel == kLogLevel_All) {
            av_log_set_callback(ffmpegLogCallback);
        }

        decoder->videoCallback = (VideoCallback)videoCallback;
        decoder->audioCallback = (AudioCallback)audioCallback;
        decoder->requestCallback = (RequestCallback)requestCallback;

        if (decoder->multiStream) {
            ret = openStreamDecoder(&decoder->videoStream);
            if (ret != kErrorCode_Success) {
                break;
            }

            ret = openStreamDecoder(&decoder->audioStream);
            if (ret != kErrorCode_Success) {
                break;
            }

            av_seek_frame(decoder->videoStream.formatContext, -1, 0, AVSEEK_FLAG_BACKWARD);
            av_seek_frame(decoder->audioStream.formatContext, -1, 0, AVSEEK_FLAG_BACKWARD);

            decoder->videoStream.frameSize = avpicture_get_size(
                decoder->videoStream.codecContext->pix_fmt,
                decoder->videoStream.codecContext->width,
                decoder->videoStream.codecContext->height);

            decoder->videoStream.bufferCapacity = 3 * decoder->videoStream.frameSize;
            decoder->videoStream.frameBuffer = (unsigned char *)av_mallocz(decoder->videoStream.bufferCapacity);
            if (decoder->videoStream.frameBuffer == NULL) {
                ret = kErrorCode_Invalid_State;
                break;
            }

            if (decoder->avFrame == NULL) {
                decoder->avFrame = av_frame_alloc();
            }

            if (decoder->avFrame == NULL) {
                ret = kErrorCode_Invalid_State;
                break;
            }

            int64_t duration = decoder->videoStream.formatContext->duration;
            if (duration <= 0 && decoder->audioStream.formatContext != NULL) {
                duration = decoder->audioStream.formatContext->duration;
            }

            params[0] = duration > 0 ? (int)(1000 * (duration + 5000) / AV_TIME_BASE) : 0;
            params[1] = decoder->videoStream.codecContext->pix_fmt;
            params[2] = decoder->videoStream.codecContext->width;
            params[3] = decoder->videoStream.codecContext->height;
            params[4] = decoder->audioStream.codecContext->sample_fmt;
            params[5] = decoder->audioStream.codecContext->channels;
            params[6] = decoder->audioStream.codecContext->sample_rate;
        } else {
            decoder->avformatContext = avformat_alloc_context();
            decoder->customIoBuffer = (unsigned char*)av_mallocz(kCustomIoBufferSize);
            if (decoder->customIoBuffer == NULL) {
                ret = kErrorCode_Invalid_State;
                break;
            }

            AVIOContext* ioContext = avio_alloc_context(
                decoder->customIoBuffer,
                kCustomIoBufferSize,
                0,
                NULL,
                readCallback,
                NULL,
                seekCallback);
            if (ioContext == NULL) {
                ret = kErrorCode_FFmpeg_Error;
                simpleLog("avio_alloc_context failed.");
                break;
            }

            decoder->avformatContext->pb = ioContext;
            decoder->avformatContext->flags = AVFMT_FLAG_CUSTOM_IO;

            r = avformat_open_input(&decoder->avformatContext, NULL, NULL, NULL);
            if (r != 0) {
                ret = kErrorCode_FFmpeg_Error;
                char err_info[32] = { 0 };
                av_strerror(r, err_info, 32);
                simpleLog("avformat_open_input failed %d %s.", r, err_info);
                break;
            }

            r = avformat_find_stream_info(decoder->avformatContext, NULL);
            if (r != 0) {
                ret = kErrorCode_FFmpeg_Error;
                simpleLog("av_find_stream_info failed %d.", r);
                break;
            }

            for (i = 0; i < decoder->avformatContext->nb_streams; i++) {
                decoder->avformatContext->streams[i]->discard = AVDISCARD_DEFAULT;
            }

            r = openCodecContext(
                decoder->avformatContext,
                AVMEDIA_TYPE_VIDEO,
                &decoder->videoStream.streamIdx,
                &decoder->videoStream.codecContext);
            if (r != 0) {
                ret = kErrorCode_FFmpeg_Error;
                simpleLog("Open video codec context failed %d.", r);
                break;
            }

            r = openCodecContext(
                decoder->avformatContext,
                AVMEDIA_TYPE_AUDIO,
                &decoder->audioStream.streamIdx,
                &decoder->audioStream.codecContext);
            if (r != 0) {
                ret = kErrorCode_FFmpeg_Error;
                simpleLog("Open audio codec context failed %d.", r);
                break;
            }

            decoder->videoStream.timeBase = decoder->avformatContext->streams[decoder->videoStream.streamIdx]->time_base;
            decoder->audioStream.timeBase = decoder->avformatContext->streams[decoder->audioStream.streamIdx]->time_base;
            decoder->videoStream.opened = 1;
            decoder->audioStream.opened = 1;
            decoder->videoStream.reachedEof = 0;
            decoder->audioStream.reachedEof = 0;
            decoder->videoStream.hasDecodedFrame = 0;
            decoder->audioStream.hasDecodedFrame = 0;

            av_seek_frame(decoder->avformatContext, -1, 0, AVSEEK_FLAG_BACKWARD);

            decoder->videoStream.frameSize = avpicture_get_size(
                decoder->videoStream.codecContext->pix_fmt,
                decoder->videoStream.codecContext->width,
                decoder->videoStream.codecContext->height);

            decoder->videoStream.bufferCapacity = 3 * decoder->videoStream.frameSize;
            decoder->videoStream.frameBuffer = (unsigned char *)av_mallocz(decoder->videoStream.bufferCapacity);
            decoder->avFrame = av_frame_alloc();
            if (decoder->videoStream.frameBuffer == NULL || decoder->avFrame == NULL) {
                ret = kErrorCode_Invalid_State;
                break;
            }

            params[0] = decoder->avformatContext->duration > 0 ?
                1000 * (decoder->avformatContext->duration + 5000) / AV_TIME_BASE : 0;
            params[1] = decoder->videoStream.codecContext->pix_fmt;
            params[2] = decoder->videoStream.codecContext->width;
            params[3] = decoder->videoStream.codecContext->height;
            params[4] = decoder->audioStream.codecContext->sample_fmt;
            params[5] = decoder->audioStream.codecContext->channels;
            params[6] = decoder->audioStream.codecContext->sample_rate;
        }

        enum AVSampleFormat sampleFmt = decoder->audioStream.codecContext->sample_fmt;
        if (av_sample_fmt_is_planar(sampleFmt)) {
            params[4] = av_get_packed_sample_fmt(sampleFmt);
        }

        if (paramArray != NULL && paramCount > 0) {
            for (int j = 0; j < paramCount && j < 7; ++j) {
                paramArray[j] = params[j];
            }
        }

        simpleLog("Decoder opened, duration %ds, picture size %d.", params[0], decoder->videoStream.frameSize);
    } while (0);

    if (ret != kErrorCode_Success && decoder != NULL) {
        if (decoder->multiStream) {
            if (decoder->videoStream.codecContext != NULL) {
                closeCodecContext(decoder->videoStream.formatContext, decoder->videoStream.codecContext, decoder->videoStream.streamIdx);
                decoder->videoStream.codecContext = NULL;
            }

            if (decoder->audioStream.codecContext != NULL) {
                closeCodecContext(decoder->audioStream.formatContext, decoder->audioStream.codecContext, decoder->audioStream.streamIdx);
                decoder->audioStream.codecContext = NULL;
            }

            releaseDecoderStream(&decoder->videoStream);
            releaseDecoderStream(&decoder->audioStream);
        } else {
            if (decoder->avformatContext != NULL) {
                if (decoder->videoStream.codecContext != NULL) {
                    closeCodecContext(decoder->avformatContext, decoder->videoStream.codecContext, decoder->videoStream.streamIdx);
                    decoder->videoStream.codecContext = NULL;
                }

                if (decoder->audioStream.codecContext != NULL) {
                    closeCodecContext(decoder->avformatContext, decoder->audioStream.codecContext, decoder->audioStream.streamIdx);
                    decoder->audioStream.codecContext = NULL;
                }

                avformat_close_input(&decoder->avformatContext);
            }

            releaseDecoderStream(&decoder->videoStream);
            releaseDecoderStream(&decoder->audioStream);
            if (decoder->customIoBuffer != NULL) {
                av_freep(&decoder->customIoBuffer);
            }
        }
        av_freep(&decoder);
    }
    return ret;
}

ErrorCode closeDecoder() {
    ErrorCode ret = kErrorCode_Success;
    do {
        if (decoder == NULL) {
            break;
        }

        if (decoder->multiStream) {
            if (decoder->videoStream.codecContext != NULL) {
                closeCodecContext(decoder->videoStream.formatContext, decoder->videoStream.codecContext, decoder->videoStream.streamIdx);
                decoder->videoStream.codecContext = NULL;
                simpleLog("Video codec context closed.");
            }

            if (decoder->videoStream.formatContext != NULL) {
                avformat_close_input(&decoder->videoStream.formatContext);
            }

            releaseDecoderStream(&decoder->videoStream);

            if (decoder->audioStream.codecContext != NULL) {
                closeCodecContext(decoder->audioStream.formatContext, decoder->audioStream.codecContext, decoder->audioStream.streamIdx);
                decoder->audioStream.codecContext = NULL;
                simpleLog("Audio codec context closed.");
            }

            if (decoder->audioStream.formatContext != NULL) {
                avformat_close_input(&decoder->audioStream.formatContext);
            }

            releaseDecoderStream(&decoder->audioStream);
        } else {
            if (decoder->avformatContext == NULL) {
                break;
            }

            if (decoder->videoStream.codecContext != NULL) {
                closeCodecContext(decoder->avformatContext, decoder->videoStream.codecContext, decoder->videoStream.streamIdx);
                decoder->videoStream.codecContext = NULL;
                simpleLog("Video codec context closed.");
            }

            if (decoder->audioStream.codecContext != NULL) {
                closeCodecContext(decoder->avformatContext, decoder->audioStream.codecContext, decoder->audioStream.streamIdx);
                decoder->audioStream.codecContext = NULL;
                simpleLog("Audio codec context closed.");
            }

            AVIOContext *pb = decoder->avformatContext->pb;
            if (pb != NULL) {
                if (pb->buffer != NULL) {
                    av_freep(&pb->buffer);
                    decoder->customIoBuffer = NULL;
                }
                av_freep(&decoder->avformatContext->pb);
                simpleLog("IO context released.");
            }

            avformat_close_input(&decoder->avformatContext);
            decoder->avformatContext = NULL;
            simpleLog("Input closed.");

            releaseDecoderStream(&decoder->videoStream);
            releaseDecoderStream(&decoder->audioStream);
            if (decoder->customIoBuffer != NULL) {
                av_freep(&decoder->customIoBuffer);
            }
        }

        if (decoder->avFrame != NULL) {
            av_freep(&decoder->avFrame);
        }
        simpleLog("All buffer released.");
    } while (0);
    return ret;
}

int sendData(int track, unsigned char *buff, int size) {
    int ret = 0;
    int64_t leftBytes = 0;
    int canWriteBytes = 0;
    do {
        if (decoder == NULL) {
            ret = -1;
            break;
        }

        if (buff == NULL || size == 0) {
            ret = -2;
            break;
        }

        if (decoder->multiStream) {
            DecoderStream *stream = track == TRACK_TYPE_AUDIO ? &decoder->audioStream : &decoder->videoStream;
            ret = streamWrite(stream, buff, size);
        } else {
            ret = decoder->isStream ? writeToFifo(buff, size) : writeToFile(buff, size);
        }
    } while (0);
    return ret;
}

ErrorCode decodeOnePacket() {
    if (decoder == NULL) {
        return kErrorCode_Invalid_State;
    }

    if (decoder->multiStream) {
        DecoderStream *stream = selectNextStreamForDecode();
        if (stream == NULL) {
            return kErrorCode_Eof;
        }

        ErrorCode ret = decodeStreamPacket(stream);
        if (ret == kErrorCode_Eof) {
            if (!decoder->videoStream.reachedEof || !decoder->audioStream.reachedEof) {
                ret = kErrorCode_Success;
            }
        }
        return ret;
    }

    if (decoder->avformatContext == NULL) {
        return kErrorCode_Invalid_State;
    }

    if (getAailableDataSize() <= 0) {
        return kErrorCode_Invalid_State;
    }

    ErrorCode ret = kErrorCode_Success;
    int decodedLen = 0;
    int r = 0;
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    r = av_read_frame(decoder->avformatContext, &packet);
    if (r == AVERROR_EOF) {
        ret = kErrorCode_Eof;
        goto end;
    }

    if (r < 0 || packet.size == 0) {
        ret = kErrorCode_FFmpeg_Error;
        goto end;
    }

    DecoderStream *targetStream = NULL;
    if (packet.stream_index == decoder->videoStream.streamIdx) {
        targetStream = &decoder->videoStream;
    } else if (packet.stream_index == decoder->audioStream.streamIdx) {
        targetStream = &decoder->audioStream;
    } else {
        goto end;
    }

    do {
        ret = decodePacket(targetStream, &packet, &decodedLen);
        if (ret != kErrorCode_Success) {
            break;
        }

        if (decodedLen <= 0) {
            break;
        }

        packet.data += decodedLen;
        packet.size -= decodedLen;
    } while (packet.size > 0);

end:
    av_packet_unref(&packet);
    return ret;
}

ErrorCode seekTo(int ms, int accurateSeek) {
    int64_t pts = (int64_t)ms * 1000;
    decoder->accurateSeek = accurateSeek;
    decoder->beginTimeOffset = (double)ms / 1000;

    if (decoder == NULL) {
        return kErrorCode_Invalid_State;
    }

    if (decoder->multiStream) {
        int retVideo = avformat_seek_file(decoder->videoStream.formatContext,
                                          -1,
                                          INT64_MIN,
                                          pts,
                                          pts,
                                          AVSEEK_FLAG_BACKWARD);
        int retAudio = avformat_seek_file(decoder->audioStream.formatContext,
                                          -1,
                                          INT64_MIN,
                                          pts,
                                          pts,
                                          AVSEEK_FLAG_BACKWARD);
        if (retVideo < 0 || retAudio < 0) {
            return kErrorCode_FFmpeg_Error;
        }

        if (decoder->videoStream.codecContext != NULL) {
            avcodec_flush_buffers(decoder->videoStream.codecContext);
        }

        if (decoder->audioStream.codecContext != NULL) {
            avcodec_flush_buffers(decoder->audioStream.codecContext);
        }

        decoder->videoStream.reachedEof = 0;
        decoder->audioStream.reachedEof = 0;
        decoder->videoStream.hasDecodedFrame = 0;
        decoder->audioStream.hasDecodedFrame = 0;
        return kErrorCode_Success;
    }

    int ret = avformat_seek_file(decoder->avformatContext,
                                 -1,
                                 INT64_MIN,
                                 pts,
                                 pts,
                                 AVSEEK_FLAG_BACKWARD);
    simpleLog("Native seek to %d return %d %d.", ms, ret, decoder->accurateSeek);
    if (ret == -1) {
        return kErrorCode_FFmpeg_Error;
    } else {
        if (decoder->videoStream.codecContext != NULL) {
            avcodec_flush_buffers(decoder->videoStream.codecContext);
        }

        if (decoder->audioStream.codecContext != NULL) {
            avcodec_flush_buffers(decoder->audioStream.codecContext);
        }

        // Trigger seek callback
        AVPacket packet;
        av_init_packet(&packet);
        av_read_frame(decoder->avformatContext, &packet);
        return kErrorCode_Success;
    }
}

int main() {
    //simpleLog("Native loaded.");
    return 0;
}

#ifdef __cplusplus
}
#endif
