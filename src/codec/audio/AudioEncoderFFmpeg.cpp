/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2016 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV (from 2015)

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

#include "QtAV/AudioEncoder.h"
#include "QtAV/private/AVEncoder_p.h"
#include "QtAV/private/AVCompat.h"
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include "QtAV/version.h"
#include "utils/Logger.h"

/*!
 * options (properties) are from libavcodec/options_table.h
 * enum name here must convert to lower case to fit the names in avcodec. done in AVEncoder.setOptions()
 * Don't use lower case here because the value name may be "default" in avcodec which is a keyword of C++
 */

namespace QtAV {

class AudioEncoderFFmpegPrivate;
class AudioEncoderFFmpeg Q_DECL_FINAL: public AudioEncoder
{
    DPTR_DECLARE_PRIVATE(AudioEncoderFFmpeg)
public:
    AudioEncoderFFmpeg();
    AudioEncoderId id() const Q_DECL_OVERRIDE;
    bool encode(const AudioFrame &frame = AudioFrame()) Q_DECL_OVERRIDE;
};

static const AudioEncoderId AudioEncoderId_FFmpeg = mkid::id32base36_6<'F', 'F', 'm', 'p', 'e', 'g'>::value;
FACTORY_REGISTER(AudioEncoder, FFmpeg, "FFmpeg")

class AudioEncoderFFmpegPrivate Q_DECL_FINAL: public AudioEncoderPrivate
{
public:
    AudioEncoderFFmpegPrivate()
        : AudioEncoderPrivate()
    {
        // avcodec_register_all();
        // NULL: codec-specific defaults won't be initialized, which may result in suboptimal default settings (this is important mainly for encoders, e.g. libx264).
        avctx = avcodec_alloc_context3(NULL);
    }
    bool open() Q_DECL_OVERRIDE;
    bool close() Q_DECL_OVERRIDE;

    QByteArray buffer;
};

bool AudioEncoderFFmpegPrivate::open()
{
    if (codec_name.isEmpty()) {
        // copy ctx from muxer by copyAVCodecContext
        const AVCodec *codec = avcodec_find_encoder(avctx->codec_id);
        AV_ENSURE_OK(avcodec_open2(avctx, codec, &dict), false);
        return true;
    }
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name.toUtf8().constData());
    if (!codec) {
        const AVCodecDescriptor* cd = avcodec_descriptor_get_by_name(codec_name.toUtf8().constData());
        if (cd) {
            codec = avcodec_find_encoder(cd->id);
        }
    }
    if (!codec) {
        qWarning() << "Can not find encoder for codec " << codec_name;
        return false;
    }
    if (avctx) {
        avcodec_free_context(&avctx);
        avctx = 0;
    }
    avctx = avcodec_alloc_context3(codec);

    // reset format_used to user defined format. important to update default format if format is invalid
    format_used = format;
    if (format.sampleRate() <= 0) {
        if (codec->supported_samplerates) {
            qDebug("use first supported sample rate: %d", codec->supported_samplerates[0]);
            format_used.setSampleRate(codec->supported_samplerates[0]);
        } else {
            qWarning("sample rate and supported sample rate are not set. use 44100");
            format_used.setSampleRate(44100);
        }
    }
    if (format.sampleFormat() == AudioFormat::SampleFormat_Unknown) {
        if (codec->sample_fmts) {
            qDebug("use first supported sample format: %d", codec->sample_fmts[0]);
            format_used.setSampleFormatFFmpeg((int)codec->sample_fmts[0]);
        } else {
            qWarning("sample format and supported sample format are not set. use s16");
            format_used.setSampleFormat(AudioFormat::SampleFormat_Signed16);
        }
    }
    if (format.channelLayout() == AudioFormat::ChannelLayout_Unsupported) {
        if (codec->channel_layouts) {
            char cl[128];
            av_get_channel_layout_string(cl, sizeof(cl), -1, codec->channel_layouts[0]); //TODO: ff version
            qDebug("use first supported channel layout: %s", cl);
            format_used.setChannelLayoutFFmpeg((qint64)codec->channel_layouts[0]);
        } else {
            qWarning("channel layout and supported channel layout are not set. use stereo");
            format_used.setChannelLayout(AudioFormat::ChannelLayout_Stereo);
        }
    }
    avctx->sample_fmt = (AVSampleFormat)format_used.sampleFormatFFmpeg();
    avctx->channel_layout = format_used.channelLayoutFFmpeg();
    avctx->channels = format_used.channels();
    avctx->sample_rate = format_used.sampleRate();
    avctx->bits_per_raw_sample = format_used.bytesPerSample()*8;

    /// set the time base. TODO
    avctx->time_base.num = 1;
    avctx->time_base.den = format_used.sampleRate();

    avctx->bit_rate = bit_rate;
    qDebug() << format_used;

    /** Allow the use of the experimental AAC encoder */
    avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    av_dict_set(&dict, "strict", "-2", 0); //aac, vorbis
    applyOptionsForContext();
    // avctx->frame_size will be set in avcodec_open2
    AV_ENSURE_OK(avcodec_open2(avctx, codec, &dict), false);
    // from mpv ao_lavc
    int pcm_hack = 0;
    int buffer_size = 0;
    frame_size = avctx->frame_size;
    if (frame_size <= 1)
        pcm_hack = av_get_bits_per_sample(avctx->codec_id)/8;
    if (pcm_hack) {
        frame_size = 16384; // "enough"
        buffer_size = frame_size*pcm_hack*format_used.channels()*2+200;
    } else {
        buffer_size = frame_size*format_used.bytesPerSample()*format_used.channels()*2+200;
    }
    if (buffer_size < AV_INPUT_BUFFER_MIN_SIZE)
        buffer_size = AV_INPUT_BUFFER_MIN_SIZE;
    buffer.resize(buffer_size);
    return true;
}

bool AudioEncoderFFmpegPrivate::close()
{
    AV_ENSURE_OK(avcodec_close(avctx), false);
    return true;
}


AudioEncoderFFmpeg::AudioEncoderFFmpeg()
    : AudioEncoder(*new AudioEncoderFFmpegPrivate())
{
}

AudioEncoderId AudioEncoderFFmpeg::id() const
{
    return AudioEncoderId_FFmpeg;
}

bool AudioEncoderFFmpeg::encode(const AudioFrame &frame)
{
    DPTR_D(AudioEncoderFFmpeg);

    AVFrame *f = nullptr;
    if (frame.isValid()) {
        f = av_frame_alloc();
        if (!f) {
            qWarning("Failed to allocate AVFrame");
            return false;
        }

        const AudioFormat fmt(frame.format());
        f->format = fmt.sampleFormatFFmpeg();
        f->channel_layout = fmt.channelLayoutFFmpeg();
        f->nb_samples = d.frame_size;
        f->pts = int64_t(frame.timestamp() * fmt.sampleRate()); // 设置 PTS

        const int nb_planes = frame.planeCount();
        const int sample_stride = fmt.isPlanar() ? fmt.bytesPerSample() : fmt.bytesPerSample() * fmt.channels();
        for (int i = 0; i < nb_planes; ++i) {
            f->linesize[i] = f->nb_samples * sample_stride;
            f->extended_data[i] = (uint8_t*)frame.constBits(i);
        }

        // 设置帧的采样率和通道布局
        f->sample_rate = fmt.sampleRate();
        f->channels = fmt.channels();
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    int ret = 0;

    // 发送帧到编码器
    if (f) {
        ret = avcodec_send_frame(d.avctx, f);
        if (ret < 0) {
            qWarning("Error sending frame to encoder: %s", av_err2str(ret));
            av_frame_free(&f);
            return false;
        }
    } else {
        // 发送 NULL 帧表示 EOF
        ret = avcodec_send_frame(d.avctx, nullptr);
        if (ret < 0) {
            qWarning("Error sending EOF frame to encoder: %s", av_err2str(ret));
            return false;
        }
    }

    // 接收编码后的数据包
    ret = avcodec_receive_packet(d.avctx, &pkt);
    if (ret == AVERROR(EAGAIN)) {
        // 需要更多帧
        qWarning("Encoder needs more frames");
        d.packet = Packet();
        return frame.isValid();
    } else if (ret == AVERROR_EOF) {
        // 编码结束
        qWarning("Encoder reached EOF");
        d.packet = Packet();
        return false;
    } else if (ret < 0) {
        qWarning("Error receiving packet from encoder: %s", av_err2str(ret));
        return false;
    }

    // 成功编码一个数据包
    d.packet = Packet::fromAVPacket(&pkt, av_q2d(d.avctx->time_base));
    av_packet_unref(&pkt); // 释放数据包

    av_frame_free(&f); // 释放帧
    return true;
}

} //namespace QtAV
