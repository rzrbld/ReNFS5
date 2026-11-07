/* movie.c - see movie.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "movie.h"
#include "win32hle.h"

extern int hle_input_any_down(void) __attribute__((weak));
extern int hle_input_take_press(void) __attribute__((weak));
extern void hle_dinput_pump(void) __attribute__((weak));

#ifdef NFS5_HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#endif

static int env_off(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    return !strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no");
}

static double max_sec(void) {
    const char *v = getenv("NFS5_MOVIE_MAX_SEC");
    if (v && *v) { double d = atof(v); if (d > 0.0) return d; }
    return 0.0; /* Normal playback completes at decoder EOF, not a timer. */
}

static int trace_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("NFS5_MOVIE_TRACE") != NULL;
    return on;
}

int nfs5_movie_available(void) {
#ifdef NFS5_HAVE_FFMPEG
    const char *skip = getenv("NFS5_SKIP_MOVIES");
    if (skip && *skip && !env_off("NFS5_SKIP_MOVIES")) return 0;
    if (env_off("NFS5_PLAY_MOVIES")) return 0;
    return 1;
#else
    return 0;
#endif
}

#ifdef NFS5_HAVE_FFMPEG

/* Push decoded audio to the host mixer. Supports S16P (the EA ADPCM output). */
static int push_audio(int stream, AVFrame *af, int64_t *pushed) {
    enum AVSampleFormat fmt = (enum AVSampleFormat)af->format;
    int ch = af->ch_layout.nb_channels;
    if (stream < 0 || ch < 1 || ch > 2) return 0;
    int n = af->nb_samples;
    if (n <= 0) return 0;
    static int16_t *tmp;
    static size_t tmp_n;
    if ((size_t)n * (size_t)ch > tmp_n) {
        tmp_n = (size_t)n * (size_t)ch;
        int16_t *next = (int16_t *)realloc(tmp, tmp_n * sizeof *tmp);
        if (!next) { tmp_n = 0; return 0; }
        tmp = next;
    }
    if (fmt == AV_SAMPLE_FMT_S16P) {
        const int16_t *l = (const int16_t *)af->extended_data[0];
        const int16_t *r = ch > 1 ? (const int16_t *)af->extended_data[1] : NULL;
        for (int i = 0; i < n; i++) {
            tmp[i * ch + 0] = l[i];
            if (ch > 1) tmp[i * ch + 1] = r[i];
        }
    } else if (fmt == AV_SAMPLE_FMT_S16) {
        memcpy(tmp, af->extended_data[0], (size_t)n * (size_t)ch * sizeof *tmp);
    } else {
        return 0;
    }
    hle_audio_stream_push(stream, tmp, n);
    *pushed += n;
    return 1;
}

static int movie_play_main(const char *host_path) {
    if (!nfs5_movie_available() || !host_path || !*host_path) return -1;
    AVFormatContext *fc = NULL;
    AVCodecContext *vc = NULL, *ac = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frm = NULL;
    int frames = 0, astream = -1, vstream = -1, stream = -1;
    int64_t pushed = 0;
    double pts0 = 0.0;
    int have_pts0 = 0;
    struct timespec start;
    if (avformat_open_input(&fc, host_path, NULL, NULL) < 0) {
        if (trace_on()) fprintf(stderr, "[movie] cannot open %s\n", host_path);
        return -1;
    }
    if (avformat_find_stream_info(fc, NULL) < 0) { avformat_close_input(&fc); return -1; }
    vstream = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    astream = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vstream < 0) { avformat_close_input(&fc); return -1; }
    const AVCodec *vd = avcodec_find_decoder(fc->streams[vstream]->codecpar->codec_id);
    if (!vd) { fprintf(stderr, "[movie] %s: no eamad decoder\n", host_path); avformat_close_input(&fc); return -1; }
    vc = avcodec_alloc_context3(vd);
    if (!vc || avcodec_parameters_to_context(vc, fc->streams[vstream]->codecpar) < 0) { avcodec_free_context(&vc); avformat_close_input(&fc); return -1; }
    if (avcodec_open2(vc, vd, NULL) < 0) { avcodec_free_context(&vc); avformat_close_input(&fc); return -1; }
    if (astream >= 0) {
        const AVCodec *ad = avcodec_find_decoder(fc->streams[astream]->codecpar->codec_id);
        if (ad) {
            ac = avcodec_alloc_context3(ad);
            if (ac && avcodec_parameters_to_context(ac, fc->streams[astream]->codecpar) == 0 &&
                avcodec_open2(ac, ad, NULL) >= 0) {
                stream = hle_audio_stream_open(ac->sample_rate, ac->ch_layout.nb_channels);
            } else if (ac) { avcodec_free_context(&ac); ac = NULL; }
        }
    }
    pkt = av_packet_alloc();
    frm = av_frame_alloc();
    if (!pkt || !frm) goto done;
    clock_gettime(CLOCK_MONOTONIC, &start);
    fprintf(stderr, "[movie] playing %s (%dx%d, embedded eamad%s)\n", host_path,
            vc->width, vc->height, stream >= 0 ? " + audio" : "");
    double deadline = max_sec();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vstream) {
            if (avcodec_send_packet(vc, pkt) >= 0) {
                while (avcodec_receive_frame(vc, frm) >= 0) {
                    if (frm->width > 0 && frm->height > 0 && frm->format == AV_PIX_FMT_YUV420P) {
                        /* Wait before presentation. Keep SDL input responsive while
                         * the guest is blocked inside its movie-file open. */
                        double pts = frm->best_effort_timestamp == AV_NOPTS_VALUE ?
                            (double)frames * av_q2d(av_inv_q(av_guess_frame_rate(fc, fc->streams[vstream], frm))) :
                            frm->best_effort_timestamp * av_q2d(fc->streams[vstream]->time_base);
                        if (!have_pts0) { pts0 = pts; have_pts0 = 1; }
                        for (;;) {
                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
                            hle_screen_pump();
                            if (hle_dinput_pump) hle_dinput_pump();
                            if (frames > 8 && ((hle_input_take_press && hle_input_take_press()) ||
                                              (hle_input_any_down && hle_input_any_down()))) goto done;
                            if (pts - pts0 <= elapsed || (deadline > 0 && elapsed >= deadline)) break;
                            hle_block_begin(); usleep(2000); hle_block_end();
                        }
                        if (hle_screen_movie_yuv(frm->data[0], frm->linesize[0],
                            frm->data[1], frm->linesize[1], frm->data[2], frm->linesize[2],
                            frm->width, frm->height)) {
                            fprintf(stderr, "[movie] SDL presentation failed\n");
                            goto done;
                        }
                        frames++;
                        /* Let any button skip (the handheld pad bridge and SDL
                         * keyboard both feed the HLE key state). */
                        if (hle_dinput_pump) hle_dinput_pump();
                        if (frames > 8 && ((hle_input_take_press && hle_input_take_press()) ||
                                           (hle_input_any_down && hle_input_any_down()))) {
                            fprintf(stderr, "[movie] skipped by input after %d frames\n", frames);
                            av_packet_unref(pkt);
                            goto done;
                        }
                    }
                    av_frame_unref(frm);
                }
            }
        } else if (pkt->stream_index == astream && ac) {
            if (avcodec_send_packet(ac, pkt) >= 0) {
                while (avcodec_receive_frame(ac, frm) >= 0) {
                    push_audio(stream, frm, &pushed);
                    av_frame_unref(frm);
                }
            }
        }
        av_packet_unref(pkt);
        if (trace_on() && (frames % 30) == 0) fprintf(stderr, "[movie] %s: frame %d\n", host_path, frames);
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
            if (deadline > 0 && elapsed > deadline) {
                fprintf(stderr, "[movie] %s: wall-clock cap after %d frames\n", host_path, frames);
                break;
            }
        }
    }
    if (stream >= 0) {
        /* Completion follows decoder EOF and the remaining queued PCM. */
        while (hle_audio_stream_queued(stream) > 0) {
            hle_screen_pump();
            if (hle_dinput_pump) hle_dinput_pump();
            if (hle_input_take_press && hle_input_take_press()) break;
            hle_block_begin(); usleep(10000); hle_block_end();
        }
    }
done:
    if (stream >= 0) hle_audio_stream_close(stream);
    hle_screen_movie_end();
    av_frame_free(&frm);
    av_packet_free(&pkt);
    if (ac) avcodec_free_context(&ac);
    avcodec_free_context(&vc);
    avformat_close_input(&fc);
    fprintf(stderr, "[movie] %s: %d frames presented, %lld audio frames\n", host_path, frames,
            (long long)pushed);
    return frames;
}

int nfs5_movie_play(const char *host_path) {
    return hle_screen_run_movie(movie_play_main, host_path);
}

#else  /* !NFS5_HAVE_FFMPEG */
int nfs5_movie_play(const char *host_path) {
    (void)host_path;
    return -1;
}
#endif
