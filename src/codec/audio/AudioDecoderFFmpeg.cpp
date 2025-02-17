/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2018 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/AudioDecoder.h"
#include "QtAV/AudioResampler.h"
#include "QtAV/Packet.h"
#include "QtAV/private/AVDecoder_p.h"
#include "QtAV/private/AVCompat.h"
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include "QtAV/version.h"
#include "utils/Logger.h"

namespace QtAV {

class AudioDecoderFFmpegPrivate;
class AudioDecoderFFmpeg : public AudioDecoder
{
    Q_OBJECT
    Q_DISABLE_COPY(AudioDecoderFFmpeg)
    DPTR_DECLARE_PRIVATE(AudioDecoderFFmpeg)
    Q_PROPERTY(QString codecName READ codecName WRITE setCodecName NOTIFY codecNameChanged)
public:
    AudioDecoderFFmpeg();
    AudioDecoderId id() const Q_DECL_OVERRIDE Q_DECL_FINAL;
    virtual QString description() const Q_DECL_OVERRIDE Q_DECL_FINAL {
        const int patch = QTAV_VERSION_PATCH(avcodec_version());
        return QStringLiteral("%1 avcodec %2.%3.%4")
                .arg(patch>=100?QStringLiteral("FFmpeg"):QStringLiteral("Libav"))
                .arg(QTAV_VERSION_MAJOR(avcodec_version())).arg(QTAV_VERSION_MINOR(avcodec_version())).arg(patch);
    }
    bool decode(const Packet& packet) Q_DECL_OVERRIDE Q_DECL_FINAL;
    AudioFrame frame() Q_DECL_OVERRIDE Q_DECL_FINAL;
Q_SIGNALS:
    void codecNameChanged() Q_DECL_OVERRIDE Q_DECL_FINAL;
};

AudioDecoderId AudioDecoderId_FFmpeg = mkid::id32base36_6<'F','F','m','p','e','g'>::value;
FACTORY_REGISTER(AudioDecoder, FFmpeg, "FFmpeg")

class AudioDecoderFFmpegPrivate Q_DECL_FINAL: public AudioDecoderPrivate
{
public:
    AudioDecoderFFmpegPrivate()
        : AudioDecoderPrivate()
        , frame(av_frame_alloc())
    {
#if !AVCODEC_STATIC_REGISTER
        avcodec_register_all();
#endif
    }
    ~AudioDecoderFFmpegPrivate() {
        if (frame) {
            av_frame_free(&frame);
            frame = 0;
        }
    }

    AVFrame *frame; //set once and not change
};

AudioDecoderId AudioDecoderFFmpeg::id() const
{
    return AudioDecoderId_FFmpeg;
}

AudioDecoderFFmpeg::AudioDecoderFFmpeg()
    : AudioDecoder(*new AudioDecoderFFmpegPrivate())
{
}


bool AudioDecoderFFmpeg::decode(const Packet &packet)
{
    if (!isAvailable())
        return false;

    DPTR_D(AudioDecoderFFmpeg);
    d.decoded.clear();
    int got_frame_ptr = 0;
    int ret = 0;

    // 处理 EOF 数据包
    if (packet.isEOF()) {
        AVPacket eofpkt;
        av_init_packet(&eofpkt);
        eofpkt.data = NULL;
        eofpkt.size = 0;

        // 发送 EOF 数据包
        ret = avcodec_send_packet(d.codec_ctx, &eofpkt);
        if (ret < 0) {
            qWarning("[AudioDecoder] Error sending EOF packet: %s", av_err2str(ret));
            return false;
        }
    } else {
        // 发送普通数据包
        ret = avcodec_send_packet(d.codec_ctx, (AVPacket*)packet.asAVPacket());
        if (ret < 0) {
            qWarning("[AudioDecoder] Error sending packet: %s", av_err2str(ret));
            return false;
        }
    }

    // 接收解码后的帧
    ret = avcodec_receive_frame(d.codec_ctx, d.frame);
    if (ret == AVERROR(EAGAIN)) {
        // 需要更多数据
        return false;
    } else if (ret == AVERROR_EOF) {
        // 解码结束
        return false;
    } else if (ret < 0) {
        qWarning("[AudioDecoder] Error receiving frame: %s", av_err2str(ret));
        return false;
    }

    // 成功解码一帧
    got_frame_ptr = 1;

    // 计算未解码数据大小
    d.undecoded_size = qMin(packet.data.size() - ret, packet.data.size());

    if (!got_frame_ptr) {
        qWarning("[AudioDecoder] got_frame_ptr=false. decoded: %d, un: %d %s", ret, d.undecoded_size, av_err2str(ret));
        return !packet.isEOF();
    }

#if USE_AUDIO_FRAME
    return true;
#endif

    // 使用音频重采样器
    d.resampler->setInSampesPerChannel(d.frame->nb_samples);
    if (!d.resampler->convert((const quint8**)d.frame->extended_data)) {
        return false;
    }

    d.decoded = d.resampler->outData();
    return !d.decoded.isEmpty();
}


AudioFrame AudioDecoderFFmpeg::frame()
{
    DPTR_D(AudioDecoderFFmpeg);
    AudioFormat fmt;
    fmt.setSampleFormatFFmpeg(d.frame->format);
    fmt.setChannelLayoutFFmpeg(d.frame->channel_layout);
    fmt.setSampleRate(d.frame->sample_rate);
    if (!fmt.isValid()) {// need more data to decode to get a frame
        return AudioFrame();
    }
    AudioFrame f(fmt);
    //av_frame_get_pkt_duration ffmpeg
    f.setBits(d.frame->extended_data); // TODO: ref
    f.setBytesPerLine(d.frame->linesize[0], 0); // for correct alignment
    f.setSamplesPerChannel(d.frame->nb_samples);
    // TODO: ffplay check AVFrame.pts, pkt_pts, last_pts+nb_samples. move to AudioFrame::from(AVFrame*)
    f.setTimestamp((double)d.frame->best_effort_timestamp/1000.0);
    f.setAudioResampler(d.resampler); // TODO: remove. it's not safe if frame is shared. use a pool or detach if ref >1
    return f;
}

} //namespace QtAV
#include "AudioDecoderFFmpeg.moc"
