self.Module = {
    onRuntimeInitialized: function () {
        onWasmLoaded();
    }
};

self.importScripts("common.js");
self.importScripts("libffmpeg.js");

const kSeparateStreamDisabled = -2;

function Decoder() {
    this.logger             = new Logger("Decoder");
    this.coreLogLevel       = 1;
    this.accurateSeek       = true;
    this.wasmLoaded         = false;
    this.tmpReqQue          = [];
    this.cacheBuffer        = {};
    this.cacheSize          = {};
    this.decodeTimer        = null;
    this.videoCallback      = null;
    this.audioCallback      = null;
    this.requestCallback    = null;
    this.useSeparateTracks  = false;
}

Decoder.prototype.initDecoder = function (fileSize, chunkSize) {
    var videoSize = fileSize;
    var audioSize = kSeparateStreamDisabled;
    var videoChunk = chunkSize;
    var audioChunk = chunkSize;

    if (typeof fileSize === 'object' && fileSize !== null) {
        videoSize = typeof fileSize.v === 'number' ? fileSize.v : -1;
        audioSize = typeof fileSize.a === 'number' ? fileSize.a : -1;
        this.useSeparateTracks = true;
    } else {
        this.useSeparateTracks = false;
    }

    if (typeof chunkSize === 'object' && chunkSize !== null) {
        videoChunk = typeof chunkSize.v === 'number' ? chunkSize.v : chunkSize;
        audioChunk = typeof chunkSize.a === 'number' ? chunkSize.a : chunkSize;
    }

    var ret = Module._initDecoder(videoSize, audioSize, this.coreLogLevel);
    this.logger.logInfo("initDecoder return " + ret + ".");

    if (ret === 0) {
        this.cacheSize[kTrackVideo] = videoChunk;
        this.cacheBuffer[kTrackVideo] = Module._malloc(videoChunk);

        if (this.useSeparateTracks) {
            this.cacheSize[kTrackAudio] = audioChunk;
            this.cacheBuffer[kTrackAudio] = Module._malloc(audioChunk);
        } else {
            this.cacheSize[kTrackAudio] = 0;
            this.cacheBuffer[kTrackAudio] = null;
        }
    }

    var objData = {
        t: kInitDecoderRsp,
        e: ret
    };
    self.postMessage(objData);
};

Decoder.prototype.uninitDecoder = function () {
    var ret = Module._uninitDecoder();
    this.logger.logInfo("Uninit ffmpeg decoder return " + ret + ".");
    var self = this;
    Object.keys(this.cacheBuffer).forEach(function (track) {
        if (self.cacheBuffer[track]) {
            Module._free(self.cacheBuffer[track]);
        }
    });
    this.cacheBuffer = {};
    this.cacheSize = {};
    this.useSeparateTracks = false;
};

Decoder.prototype.openDecoder = function () {
    var paramCount = 7, paramSize = 4;
    var paramByteBuffer = Module._malloc(paramCount * paramSize);
    var ret = Module._openDecoder(paramByteBuffer, paramCount, this.videoCallback, this.audioCallback, this.requestCallback);
    this.logger.logInfo("openDecoder return " + ret);

    if (ret == 0) {
        var paramIntBuff    = paramByteBuffer >> 2;
        var paramArray      = Module.HEAP32.subarray(paramIntBuff, paramIntBuff + paramCount);
        var duration        = paramArray[0];
        var videoPixFmt     = paramArray[1];
        var videoWidth      = paramArray[2];
        var videoHeight     = paramArray[3];
        var audioSampleFmt  = paramArray[4];
        var audioChannels   = paramArray[5];
        var audioSampleRate = paramArray[6];

        var objData = {
            t: kOpenDecoderRsp,
            e: ret,
            v: {
                d: duration,
                p: videoPixFmt,
                w: videoWidth,
                h: videoHeight
            },
            a: {
                f: audioSampleFmt,
                c: audioChannels,
                r: audioSampleRate
            }
        };
        self.postMessage(objData);
    } else {
        var objData = {
            t: kOpenDecoderRsp,
            e: ret
        };
        self.postMessage(objData);
    }
    Module._free(paramByteBuffer);
};

Decoder.prototype.closeDecoder = function () {
    this.logger.logInfo("closeDecoder.");
    if (this.decodeTimer) {
        clearInterval(this.decodeTimer);
        this.decodeTimer = null;
        this.logger.logInfo("Decode timer stopped.");
    }

    var ret = Module._closeDecoder();
    this.logger.logInfo("Close ffmpeg decoder return " + ret + ".");

    var objData = {
        t: kCloseDecoderRsp,
        e: 0
    };
    self.postMessage(objData);
};

Decoder.prototype.startDecoding = function (interval) {
    //this.logger.logInfo("Start decoding.");
    if (this.decodeTimer) {
        clearInterval(this.decodeTimer);
    }
    this.decodeTimer = setInterval(this.decode, interval);
};

Decoder.prototype.pauseDecoding = function () {
    //this.logger.logInfo("Pause decoding.");
    if (this.decodeTimer) {
        clearInterval(this.decodeTimer);
        this.decodeTimer = null;
    }
};

Decoder.prototype.decode = function () {
    var ret = Module._decodeOnePacket();
    if (ret == 7) {
        self.decoder.logger.logInfo("Decoder finished.");
        self.decoder.pauseDecoding();
        var objData = {
            t: kDecodeFinishedEvt,
        };
        self.postMessage(objData);
    }

    while (ret == 9) {
        //self.decoder.logger.logInfo("One old frame");
        ret = Module._decodeOnePacket();
    }
};

Decoder.prototype.sendData = function (track, data) {
    if (typeof track !== 'number') {
        track = kTrackVideo;
    }

    if (!this.cacheBuffer.hasOwnProperty(track) || this.cacheBuffer[track] === null) {
        this.logger.logError("Cache buffer for track " + track + " not initialized.");
        return;
    }

    var typedArray = new Uint8Array(data);

    if (this.cacheSize[track] < typedArray.length) {
        Module._free(this.cacheBuffer[track]);
        this.cacheBuffer[track] = Module._malloc(typedArray.length);
        this.cacheSize[track] = typedArray.length;
    }

    Module.HEAPU8.set(typedArray, this.cacheBuffer[track]);
    Module._sendData(track, this.cacheBuffer[track], typedArray.length);
};

Decoder.prototype.seekTo = function (ms) {
    var accurateSeek = this.accurateSeek ? 1 : 0;
    var ret = Module._seekTo(ms, accurateSeek);
    var objData = {
        t: kSeekToRsp,
        r: ret
    };
    self.postMessage(objData);
};

Decoder.prototype.processReq = function (req) {
    //this.logger.logInfo("processReq " + req.t + ".");
    switch (req.t) {
        case kInitDecoderReq:
            this.initDecoder(req.s, req.c);
            break;
        case kUninitDecoderReq:
            this.uninitDecoder();
            break;
        case kOpenDecoderReq:
            this.openDecoder();
            break;
        case kCloseDecoderReq:
            this.closeDecoder();
            break;
        case kStartDecodingReq:
            this.startDecoding(req.i);
            break;
        case kPauseDecodingReq:
            this.pauseDecoding();
            break;
        case kFeedDataReq:
            this.sendData(req.r, req.d);
            break;
        case kSeekToReq:
            this.seekTo(req.ms);
            break;
        default:
            this.logger.logError("Unsupport messsage " + req.t);
    }
};

Decoder.prototype.cacheReq = function (req) {
    if (req) {
        this.tmpReqQue.push(req);
    }
};

Decoder.prototype.onWasmLoaded = function () {
    this.logger.logInfo("Wasm loaded.");
    this.wasmLoaded = true;

    this.videoCallback = Module.addFunction(function (buff, size, timestamp) {
        var outArray = Module.HEAPU8.subarray(buff, buff + size);
        var data = new Uint8Array(outArray);
        var objData = {
            t: kVideoFrame,
            s: timestamp,
            d: data
        };
        self.postMessage(objData, [objData.d.buffer]);
    }, 'viid');

    this.audioCallback = Module.addFunction(function (buff, size, timestamp) {
        var outArray = Module.HEAPU8.subarray(buff, buff + size);
        var data = new Uint8Array(outArray);
        var objData = {
            t: kAudioFrame,
            s: timestamp,
            d: data
        };
        self.postMessage(objData, [objData.d.buffer]);
    }, 'viid');

    this.requestCallback = Module.addFunction(function (track, offset, availble) {
        var objData = {
            t: kRequestDataEvt,
            k: track,
            o: offset,
            a: availble
        };
        self.postMessage(objData);
    }, 'viii');

    while (this.tmpReqQue.length > 0) {
        var req = this.tmpReqQue.shift();
        this.processReq(req);
    }
};

self.decoder = new Decoder;

self.onmessage = function (evt) {
    if (!self.decoder) {
        console.log("[ER] Decoder not initialized!");
        return;
    }

    var req = evt.data;
    if (!self.decoder.wasmLoaded) {
        self.decoder.cacheReq(req);
        self.decoder.logger.logInfo("Temp cache req " + req.t + ".");
        return;
    }

    self.decoder.processReq(req);
};

function onWasmLoaded() {
    if (self.decoder) {
        self.decoder.onWasmLoaded();
    } else {
        console.log("[ER] No decoder!");
    }
}
