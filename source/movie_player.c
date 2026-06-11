/* movie_player.c -- in-engine video playback for the intro/credits movies
 *
 * Pipeline: a decoder thread demuxes the .m4v with FFmpeg, decodes H.264
 * video into a small ring of tightly-packed YUV420 frames and streams the
 * AAC audio through an OpenAL queue. The render side runs inside the
 * eglSwapBuffers hook on the game's GL context: it picks the ring frame due
 * by the audio clock, uploads it into three luminance textures and draws a
 * letterboxed fullscreen quad with a YUV->RGB shader.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>
#include <switch.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <AL/al.h>
#include <AL/alc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#include "config.h"
#include "util.h"
#include "movie_player.h"
#include "font_atlas.h"

// the ring must be deep enough to absorb the longest run of video packets
// the muxer interleaves between audio packets (~0.5s), otherwise the decoder
// blocks on video before it can feed the audio queue and playback deadlocks
#define NUM_VFRAMES 16  // decoded video frames buffered ahead (~0.6s)
#define NUM_ABUFS 8     // OpenAL queue depth
#define ABUF_SAMPLES 4096 // stereo sample frames per OpenAL buffer (~93ms)
#define AUDIO_PREFILL 3 // queued chunks before the playback clock starts

typedef struct {
  uint8_t *y, *u, *v;
  double pts;
} VideoFrame;

static struct {
  volatile int active;
  volatile int paused;
  volatile int stop_req;
  volatile int decode_eof;

  thrd_t thread;
  int thread_running;

  AVFormatContext *fmt;
  AVCodecContext *vctx, *actx;
  SwrContext *swr;
  int vstream, astream;
  double video_tb; // video stream time base in seconds

  int width, height;

  // video frame ring: decoder produces, render consumes
  VideoFrame frames[NUM_VFRAMES];
  int frame_read, frame_write, frame_count;
  mtx_t lock;
  cnd_t can_produce;

  // audio
  int has_audio;
  int audio_chunks; // chunks queued so far
  int audio_primed; // enough buffered; the audio clock may run
  int audio_rate;
  ALuint al_source;
  ALuint al_buffers[NUM_ABUFS];
  int al_buf_free[NUM_ABUFS];
  int al_buf_samples[NUM_ABUFS];
  uint64_t samples_played; // sample frames of all unqueued buffers
  // accumulates converted s16 stereo pcm until a full AL buffer is ready
  int16_t *pcm_acc;
  int pcm_acc_samples;

  // wall clock fallback when the file has no audio track
  u64 start_tick;
  u64 pause_tick;
  u64 paused_ticks;

  // wall continuation after the audio track ended (it can be shorter than
  // the video track; the remaining frames must still play out)
  u64 tail_tick;
  double tail_base;

  int frame_shown; // at least one frame was displayed
} mp;

// GL objects survive across movies (only recreated when the size changes)
static struct {
  GLuint prog;
  GLuint tex[3];
  GLint loc_pos, loc_uv, loc_tex[3];
  int tex_w, tex_h;
  int ready;
  // subtitle overlay
  GLuint text_prog;
  GLuint text_tex;
  GLint text_loc_pos, text_loc_uv, text_loc_tex, text_loc_off, text_loc_color;
  int text_uploaded;
} gl;

// ---------------------------------------------------------------------------
// subtitles: the game pushes the current line through
// GamePlatformServices.setMovieText (one full line per call, empty string
// clears); on Android a TextView above the video view drew it
// ---------------------------------------------------------------------------

#define SUB_MAX_TEXT 512
#define SUB_MAX_LINES 4

static struct {
  once_flag once;
  mtx_t lock;
  char text[SUB_MAX_TEXT];
} sub = { .once = ONCE_FLAG_INIT };

static void sub_init_lock(void) {
  mtx_init(&sub.lock, mtx_plain);
}

// the atlas only has ASCII 32..126: decode UTF-8 and map the common
// typographic punctuation, everything else becomes '?'
static void sub_sanitize(const char *in, char *out, int out_size) {
  const unsigned char *p = (const unsigned char *)in;
  int o = 0;
  while (*p && o < out_size - 4) {
    unsigned int cp = *p;
    if (cp < 0x80) {
      p += 1;
    } else if ((cp & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((cp & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else {
      cp = '?';
      p += 1;
    }
    switch (cp) {
      case 0x2018: case 0x2019: cp = '\''; break;
      case 0x201C: case 0x201D: cp = '"'; break;
      case 0x2013: case 0x2014: cp = '-'; break;
      case 0x00A0: cp = ' '; break;
      case 0x2026: out[o++] = '.'; out[o++] = '.'; cp = '.'; break;
      case '\n': case '\r': case '\t': cp = ' '; break;
      default: break;
    }
    out[o++] = (cp >= 32 && cp < 127) ? (char)cp : '?';
  }
  out[o] = 0;
}

void movie_set_text(const char *text) {
  call_once(&sub.once, sub_init_lock);
  mtx_lock(&sub.lock);
  sub_sanitize(text ? text : "", sub.text, sizeof(sub.text));
  mtx_unlock(&sub.lock);
}

void movie_set_text_scale(int scale) {
  // the Java TextView used this for its point size; the GL overlay derives
  // its size from the screen height instead. logged in case some movie
  // turns out to need it honored.
  debugPrintf("movie: setMovieTextScale(%d)\n", scale);
}

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------

static double movie_clock(void) {
  if (mp.has_audio) {
    // hold playback until the audio queue is primed: starting the video on
    // a different clock and jumping back to the audio clock later would
    // stall the pipeline (frames already shown are ahead of the audio)
    if (!mp.audio_primed)
      return -1.0;
    ALint off = 0;
    mtx_lock(&mp.lock);
    alGetSourcei(mp.al_source, AL_SAMPLE_OFFSET, &off);
    double clk = (double)(mp.samples_played + (uint64_t)off) / (double)mp.audio_rate;
    if (mp.decode_eof) {
      // audio done but video frames may remain: continue on wall time
      ALint state = 0;
      alGetSourcei(mp.al_source, AL_SOURCE_STATE, &state);
      if (state != AL_PLAYING) {
        const u64 now = armGetSystemTick();
        if (!mp.tail_tick) {
          mp.tail_tick = now;
          mp.tail_base = clk;
        }
        clk = mp.tail_base + (double)(now - mp.tail_tick) / (double)armGetSystemTickFreq();
      }
    }
    mtx_unlock(&mp.lock);
    return clk;
  }
  const u64 now = mp.paused ? mp.pause_tick : armGetSystemTick();
  return (double)(now - mp.start_tick - mp.paused_ticks) / (double)armGetSystemTickFreq();
}

// ---------------------------------------------------------------------------
// decoder thread
// ---------------------------------------------------------------------------

static void copy_plane(uint8_t *dst, const uint8_t *src, int width, int height, int stride) {
  if (stride == width) {
    memcpy(dst, src, (size_t)width * height);
  } else {
    for (int yy = 0; yy < height; yy++)
      memcpy(dst + (size_t)yy * width, src + (size_t)yy * stride, width);
  }
}

static void push_video_frame(const AVFrame *frm) {
  mtx_lock(&mp.lock);
  while (mp.frame_count == NUM_VFRAMES && !mp.stop_req)
    cnd_wait(&mp.can_produce, &mp.lock);
  if (mp.stop_req) {
    mtx_unlock(&mp.lock);
    return;
  }
  VideoFrame *slot = &mp.frames[mp.frame_write];
  copy_plane(slot->y, frm->data[0], mp.width, mp.height, frm->linesize[0]);
  copy_plane(slot->u, frm->data[1], mp.width / 2, mp.height / 2, frm->linesize[1]);
  copy_plane(slot->v, frm->data[2], mp.width / 2, mp.height / 2, frm->linesize[2]);
  int64_t ts = frm->best_effort_timestamp;
  if (ts == AV_NOPTS_VALUE)
    ts = frm->pts;
  slot->pts = (ts == AV_NOPTS_VALUE) ? 0.0 : (double)ts * mp.video_tb;
  mp.frame_write = (mp.frame_write + 1) % NUM_VFRAMES;
  mp.frame_count++;
  mtx_unlock(&mp.lock);
}

// unqueue whatever the source has finished with; must hold mp.lock
static void reclaim_audio_locked(void) {
  ALint processed = 0;
  alGetSourcei(mp.al_source, AL_BUFFERS_PROCESSED, &processed);
  while (processed-- > 0) {
    ALuint buf = 0;
    alSourceUnqueueBuffers(mp.al_source, 1, &buf);
    for (int i = 0; i < NUM_ABUFS; i++) {
      if (mp.al_buffers[i] == buf) {
        mp.samples_played += mp.al_buf_samples[i];
        mp.al_buf_free[i] = 1;
        break;
      }
    }
  }
}

static void queue_audio_chunk(const int16_t *pcm, int samples) {
  for (;;) {
    mtx_lock(&mp.lock);
    reclaim_audio_locked();
    int idx = -1;
    for (int i = 0; i < NUM_ABUFS; i++) {
      if (mp.al_buf_free[i]) {
        idx = i;
        break;
      }
    }
    if (idx >= 0) {
      alBufferData(mp.al_buffers[idx], AL_FORMAT_STEREO16, pcm, samples * 2 * sizeof(int16_t), mp.audio_rate);
      alSourceQueueBuffers(mp.al_source, 1, &mp.al_buffers[idx]);
      mp.al_buf_free[idx] = 0;
      mp.al_buf_samples[idx] = samples;
      if (!mp.paused) {
        ALint state = 0;
        alGetSourcei(mp.al_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING)
          alSourcePlay(mp.al_source);
      }
      if (++mp.audio_chunks >= AUDIO_PREFILL)
        mp.audio_primed = 1;
      mtx_unlock(&mp.lock);
      return;
    }
    mtx_unlock(&mp.lock);
    if (mp.stop_req)
      return;
    thrd_sleep(&(struct timespec){ .tv_nsec = 5 * 1000 * 1000 }, NULL);
  }
}

static void push_audio_frame(const AVFrame *frm) {
  // convert to interleaved s16 stereo at the source rate
  static int16_t tmp[8192 * 2];
  uint8_t *outp[1] = { (uint8_t *)tmp };
  const int out = swr_convert(mp.swr, outp, 8192, (const uint8_t **)frm->data, frm->nb_samples);
  if (out <= 0)
    return;
  memcpy(mp.pcm_acc + mp.pcm_acc_samples * 2, tmp, (size_t)out * 2 * sizeof(int16_t));
  mp.pcm_acc_samples += out;
  while (mp.pcm_acc_samples >= ABUF_SAMPLES && !mp.stop_req) {
    queue_audio_chunk(mp.pcm_acc, ABUF_SAMPLES);
    mp.pcm_acc_samples -= ABUF_SAMPLES;
    memmove(mp.pcm_acc, mp.pcm_acc + ABUF_SAMPLES * 2, (size_t)mp.pcm_acc_samples * 2 * sizeof(int16_t));
  }
}

static void drain_codec(AVCodecContext *ctx, AVFrame *frm, int is_video) {
  while (avcodec_receive_frame(ctx, frm) == 0) {
    if (mp.stop_req)
      return;
    if (is_video)
      push_video_frame(frm);
    else
      push_audio_frame(frm);
  }
}

static int decoder_main(void *arg) {
  (void)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frm = av_frame_alloc();

  while (!mp.stop_req) {
    if (mp.paused) {
      thrd_sleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
      continue;
    }
    if (av_read_frame(mp.fmt, pkt) < 0)
      break; // end of file
    if (pkt->stream_index == mp.vstream) {
      if (avcodec_send_packet(mp.vctx, pkt) == 0)
        drain_codec(mp.vctx, frm, 1);
    } else if (mp.has_audio && pkt->stream_index == mp.astream) {
      if (avcodec_send_packet(mp.actx, pkt) == 0)
        drain_codec(mp.actx, frm, 0);
    }
    av_packet_unref(pkt);
  }

  if (!mp.stop_req) {
    // flush both decoders and the pcm remainder
    avcodec_send_packet(mp.vctx, NULL);
    drain_codec(mp.vctx, frm, 1);
    if (mp.has_audio) {
      avcodec_send_packet(mp.actx, NULL);
      drain_codec(mp.actx, frm, 0);
      if (mp.pcm_acc_samples > 0 && !mp.stop_req) {
        queue_audio_chunk(mp.pcm_acc, mp.pcm_acc_samples);
        mp.pcm_acc_samples = 0;
      }
    }
  }

  av_frame_free(&frm);
  av_packet_free(&pkt);
  // a movie shorter than the prefill threshold must still run its clock
  mp.audio_primed = 1;
  mp.decode_eof = 1;
  return 0;
}

// ---------------------------------------------------------------------------
// GL overlay
// ---------------------------------------------------------------------------

static const char vshader_src[] =
  "attribute vec2 aPos;\n"
  "attribute vec2 aUV;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  vUV = aUV;\n"
  "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
  "}\n";

static const char fshader_src[] =
  "precision mediump float;\n"
  "uniform sampler2D texY;\n"
  "uniform sampler2D texU;\n"
  "uniform sampler2D texV;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  float y = texture2D(texY, vUV).r;\n"
  "  float u = texture2D(texU, vUV).r - 0.5;\n"
  "  float v = texture2D(texV, vUV).r - 0.5;\n"
  "  gl_FragColor = vec4(y + 1.402 * v, y - 0.344 * u - 0.714 * v, y + 1.772 * u, 1.0);\n"
  "}\n";

static const char text_vshader_src[] =
  "attribute vec2 aPos;\n"
  "attribute vec2 aUV;\n"
  "uniform vec2 uOff;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  vUV = aUV;\n"
  "  gl_Position = vec4(aPos + uOff, 0.0, 1.0);\n"
  "}\n";

static const char text_fshader_src[] =
  "precision mediump float;\n"
  "uniform sampler2D texFont;\n"
  "uniform vec4 uColor;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * texture2D(texFont, vUV).r);\n"
  "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok)
    debugPrintf("movie: shader compile failed\n");
  return s;
}

static int gl_init(void) {
  if (gl.ready)
    return 1;
  const GLuint vs = compile_shader(GL_VERTEX_SHADER, vshader_src);
  const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fshader_src);
  gl.prog = glCreateProgram();
  glAttachShader(gl.prog, vs);
  glAttachShader(gl.prog, fs);
  glLinkProgram(gl.prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(gl.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    debugPrintf("movie: program link failed\n");
    glDeleteProgram(gl.prog);
    gl.prog = 0;
    return 0;
  }
  gl.loc_pos = glGetAttribLocation(gl.prog, "aPos");
  gl.loc_uv = glGetAttribLocation(gl.prog, "aUV");
  gl.loc_tex[0] = glGetUniformLocation(gl.prog, "texY");
  gl.loc_tex[1] = glGetUniformLocation(gl.prog, "texU");
  gl.loc_tex[2] = glGetUniformLocation(gl.prog, "texV");
  glGenTextures(3, gl.tex);

  // subtitle overlay program; the atlas itself is uploaded lazily from the
  // render path so gl_init doesn't perturb the game's texture bindings
  const GLuint tvs = compile_shader(GL_VERTEX_SHADER, text_vshader_src);
  const GLuint tfs = compile_shader(GL_FRAGMENT_SHADER, text_fshader_src);
  gl.text_prog = glCreateProgram();
  glAttachShader(gl.text_prog, tvs);
  glAttachShader(gl.text_prog, tfs);
  glLinkProgram(gl.text_prog);
  glDeleteShader(tvs);
  glDeleteShader(tfs);
  ok = 0;
  glGetProgramiv(gl.text_prog, GL_LINK_STATUS, &ok);
  if (ok) {
    gl.text_loc_pos = glGetAttribLocation(gl.text_prog, "aPos");
    gl.text_loc_uv = glGetAttribLocation(gl.text_prog, "aUV");
    gl.text_loc_tex = glGetUniformLocation(gl.text_prog, "texFont");
    gl.text_loc_off = glGetUniformLocation(gl.text_prog, "uOff");
    gl.text_loc_color = glGetUniformLocation(gl.text_prog, "uColor");
    glGenTextures(1, &gl.text_tex);
  } else {
    debugPrintf("movie: text program link failed, subtitles disabled\n");
    glDeleteProgram(gl.text_prog);
    gl.text_prog = 0;
  }

  gl.ready = 1;
  return 1;
}

static void gl_upload_frame(const VideoFrame *f) {
  const int realloc_tex = (gl.tex_w != mp.width || gl.tex_h != mp.height);
  const uint8_t *planes[3] = { f->y, f->u, f->v };
  // the planes are tightly packed and the chroma width can be odd-sized;
  // the default alignment of 4 would shear the image (and read OOB)
  GLint prev_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int i = 0; i < 3; i++) {
    const int w = i ? mp.width / 2 : mp.width;
    const int h = i ? mp.height / 2 : mp.height;
    glBindTexture(GL_TEXTURE_2D, gl.tex[i]);
    if (realloc_tex) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]);
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]);
    }
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
  if (realloc_tex) {
    gl.tex_w = mp.width;
    gl.tex_h = mp.height;
  }
}

// two triangles per glyph into verts (x,y,u,v interleaved); spaces advance
// the pen without emitting geometry
static int sub_emit_line(const char *text, int start, int len, float x, float y,
                         float gw, float gh, GLfloat *verts, int quads) {
  for (int j = 0; j < len; j++) {
    const char c = text[start + j];
    if (c == ' ')
      continue;
    const int idx = c - FONT_FIRST;
    const float u0 = (float)((idx % FONT_COLS) * FONT_CELL_W) / (float)FONT_ATLAS_W;
    const float v0 = (float)((idx / FONT_COLS) * FONT_CELL_H) / (float)FONT_ATLAS_H;
    const float u1 = u0 + (float)FONT_CELL_W / (float)FONT_ATLAS_W;
    const float v1 = v0 + (float)FONT_CELL_H / (float)FONT_ATLAS_H;
    const float gx = x + j * gw;
    const float x0 = gx * 2.0f / (float)screen_width - 1.0f;
    const float x1 = (gx + gw) * 2.0f / (float)screen_width - 1.0f;
    const float y0 = 1.0f - y * 2.0f / (float)screen_height;
    const float y1 = 1.0f - (y + gh) * 2.0f / (float)screen_height;
    const GLfloat quad[24] = {
      x0, y0, u0, v0,  x1, y0, u1, v0,  x0, y1, u0, v1,
      x1, y0, u1, v0,  x1, y1, u1, v1,  x0, y1, u0, v1,
    };
    memcpy(verts + quads * 24, quad, sizeof(quad));
    quads++;
  }
  return quads;
}

static void movie_render_text(void) {
  if (!gl.text_prog)
    return;

  call_once(&sub.once, sub_init_lock);
  char text[SUB_MAX_TEXT];
  mtx_lock(&sub.lock);
  memcpy(text, sub.text, sizeof(text));
  mtx_unlock(&sub.lock);
  if (!text[0])
    return;

  // glyph size scales with the screen; the aspect comes from the atlas cell
  const float gh = (float)screen_height / 16.0f;
  const float gw = gh * (float)FONT_CELL_W / (float)FONT_CELL_H;

  // word wrap at ~92% screen width
  int max_cols = (int)((float)screen_width * 0.92f / gw);
  if (max_cols < 8)
    max_cols = 8;
  const int tlen = (int)strlen(text);
  int line_start[SUB_MAX_LINES], line_len[SUB_MAX_LINES], nlines = 0;
  int pos = 0;
  while (pos < tlen && nlines < SUB_MAX_LINES) {
    while (pos < tlen && text[pos] == ' ')
      pos++;
    if (pos >= tlen)
      break;
    int end = pos + max_cols;
    if (end >= tlen) {
      end = tlen;
    } else {
      int brk = end;
      while (brk > pos && text[brk] != ' ')
        brk--;
      if (brk > pos)
        end = brk; // break at the last space, else hard-break mid-word
    }
    line_start[nlines] = pos;
    line_len[nlines] = end - pos;
    nlines++;
    pos = end;
  }
  if (!nlines)
    return;

  static GLfloat verts[SUB_MAX_TEXT * 24];
  int quads = 0;
  const float y_top = (float)screen_height * 0.96f - nlines * gh;
  for (int i = 0; i < nlines; i++) {
    const float line_w = line_len[i] * gw;
    const float x = ((float)screen_width - line_w) * 0.5f;
    quads = sub_emit_line(text, line_start[i], line_len[i], x, y_top + i * gh, gw, gh, verts, quads);
  }
  if (!quads)
    return;

  // upload the atlas on first use (inside the caller's state save/restore)
  if (!gl.text_uploaded) {
    GLint prev_align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl.text_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, FONT_ATLAS_W, FONT_ATLAS_H, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, font_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    gl.text_uploaded = 1;
  }

  // the caller saves/restores program, textures and the blend enable; the
  // blend func is saved here since only the text pass changes it
  GLint bsrc_rgb, bdst_rgb, bsrc_a, bdst_a;
  glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(gl.text_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl.text_tex);
  glUniform1i(gl.text_loc_tex, 0);
  glEnableVertexAttribArray(gl.text_loc_pos);
  glEnableVertexAttribArray(gl.text_loc_uv);
  glVertexAttribPointer(gl.text_loc_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
  glVertexAttribPointer(gl.text_loc_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);

  // drop shadow first, then the text itself
  glUniform2f(gl.text_loc_off, 4.0f / (float)screen_width, -4.0f / (float)screen_height);
  glUniform4f(gl.text_loc_color, 0.0f, 0.0f, 0.0f, 0.9f);
  glDrawArrays(GL_TRIANGLES, 0, quads * 6);
  glUniform2f(gl.text_loc_off, 0.0f, 0.0f);
  glUniform4f(gl.text_loc_color, 1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 0, quads * 6);

  glDisableVertexAttribArray(gl.text_loc_pos);
  glDisableVertexAttribArray(gl.text_loc_uv);
  glDisable(GL_BLEND); // back to the state movie_render set up
  glBlendFuncSeparate(bsrc_rgb, bdst_rgb, bsrc_a, bdst_a);
}

static void movie_render(void) {
  const int gl_ok = gl_init();

  // adopt the newest ring frame that is due by the clock. this must happen
  // even when GL setup failed: the decoder blocks on a full ring, and
  // playback (and the game waiting on it) can only progress if we consume
  const double now = movie_clock();
  int adopted = -1;
  mtx_lock(&mp.lock);
  // show the first frame right away, even while the audio is still priming
  if (!mp.frame_shown && mp.frame_count > 0) {
    adopted = mp.frame_read;
    mp.frame_read = (mp.frame_read + 1) % NUM_VFRAMES;
    mp.frame_count--;
  }
  while (mp.frame_count > 0 && mp.frames[mp.frame_read].pts <= now) {
    adopted = mp.frame_read;
    mp.frame_read = (mp.frame_read + 1) % NUM_VFRAMES;
    mp.frame_count--;
    // stop early so we only skip frames when we're actually behind
    if (mp.frame_count == 0 || mp.frames[mp.frame_read].pts > now)
      break;
  }
  if (adopted >= 0) {
    // upload before releasing the lock: the slot becomes writable again
    // for the decoder as soon as frame_count drops
    if (gl_ok)
      gl_upload_frame(&mp.frames[adopted]);
    cnd_signal(&mp.can_produce);
    mp.frame_shown = 1;
  }
  mtx_unlock(&mp.lock);

  if (!gl_ok || !mp.frame_shown)
    return; // nothing to draw (yet), keep the game's output

  // save the GL state we are about to touch
  GLint prev_prog, prev_active, prev_tex[3], prev_array_buf, prev_viewport[4];
  GLfloat prev_clear[4];
  GLboolean prev_blend = glIsEnabled(GL_BLEND);
  GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex[i]);
  }

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glViewport(0, 0, screen_width, screen_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  // letterboxed quad in clip space
  float sx = 1.0f, sy = 1.0f;
  const float vid_aspect = (float)mp.width / (float)mp.height;
  const float scr_aspect = (float)screen_width / (float)screen_height;
  if (vid_aspect < scr_aspect)
    sx = vid_aspect / scr_aspect;
  else
    sy = scr_aspect / vid_aspect;

  const GLfloat pos[8] = { -sx, -sy, sx, -sy, -sx, sy, sx, sy };
  const GLfloat uv[8] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

  glUseProgram(gl.prog);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, gl.tex[i]);
    glUniform1i(gl.loc_tex[i], i);
  }
  glEnableVertexAttribArray(gl.loc_pos);
  glEnableVertexAttribArray(gl.loc_uv);
  glVertexAttribPointer(gl.loc_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
  glVertexAttribPointer(gl.loc_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(gl.loc_pos);
  glDisableVertexAttribArray(gl.loc_uv);

  movie_render_text();

  // restore
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, prev_tex[i]);
  }
  glActiveTexture(prev_active);
  glUseProgram(prev_prog);
  glBindBuffer(GL_ARRAY_BUFFER, prev_array_buf);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
  glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
  if (prev_blend) glEnable(GL_BLEND);
  if (prev_depth) glEnable(GL_DEPTH_TEST);
  if (prev_scissor) glEnable(GL_SCISSOR_TEST);
  if (prev_cull) glEnable(GL_CULL_FACE);
}

static volatile unsigned int game_swaps = 0;

unsigned int eglSwapBuffersHook(void *display, void *surface) {
  if (mp.active)
    movie_render();
  game_swaps++;
  return eglSwapBuffers((EGLDisplay)display, (EGLSurface)surface);
}

// the game stops rendering entirely while it waits for a movie (on android
// a Java video view covered the screen), so nobody would call
// eglSwapBuffers and playback would stall. the main loop calls this every
// frame: when a movie is active and the game didn't present since the last
// tick, render and present it ourselves. the swap also blocks on vsync,
// which paces the otherwise idle wait loop.
void movie_main_loop_tick(void) {
  static unsigned int last_swaps = 0;
  if (!mp.active) {
    last_swaps = game_swaps;
    return;
  }
  if (game_swaps != last_swaps) {
    last_swaps = game_swaps;
    return; // the game is presenting; the hook handles the overlay
  }
  EGLDisplay d = eglGetCurrentDisplay();
  EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
  if (d == EGL_NO_DISPLAY || s == EGL_NO_SURFACE) {
    // no way to present; don't let the game wait on us forever
    debugPrintf("movie: no current EGL surface, aborting playback\n");
    movie_stop();
    return;
  }
  movie_render();
  eglSwapBuffers(d, s);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

static void free_session(void) {
  for (int i = 0; i < NUM_VFRAMES; i++) {
    free(mp.frames[i].y);
    free(mp.frames[i].u);
    free(mp.frames[i].v);
  }
  free(mp.pcm_acc);
  if (mp.has_audio) {
    alSourceStop(mp.al_source);
    ALint queued = 0;
    alGetSourcei(mp.al_source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) {
      ALuint buf = 0;
      alSourceUnqueueBuffers(mp.al_source, 1, &buf);
    }
    alDeleteSources(1, &mp.al_source);
    alDeleteBuffers(NUM_ABUFS, mp.al_buffers);
  }
  if (mp.swr)
    swr_free(&mp.swr);
  if (mp.vctx)
    avcodec_free_context(&mp.vctx);
  if (mp.actx)
    avcodec_free_context(&mp.actx);
  if (mp.fmt)
    avformat_close_input(&mp.fmt);
  mtx_destroy(&mp.lock);
  cnd_destroy(&mp.can_produce);
  memset(&mp, 0, sizeof(mp));
}

void movie_stop(void) {
  if (!mp.active)
    return;
  mp.stop_req = 1;
  mtx_lock(&mp.lock);
  cnd_broadcast(&mp.can_produce);
  mtx_unlock(&mp.lock);
  if (mp.thread_running) {
    int res;
    thrd_join(mp.thread, &res);
  }
  free_session();
  movie_set_text(NULL);
  debugPrintf("movie: stopped\n");
}

static int open_codec(AVCodecContext **out, AVStream *stream, int threads) {
  const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec)
    return -1;
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx)
    return -1;
  if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 ||
      (ctx->thread_count = threads, avcodec_open2(ctx, codec, NULL)) < 0) {
    avcodec_free_context(&ctx);
    return -1;
  }
  *out = ctx;
  return 0;
}

static int mp_skippable = 0;

int movie_play(const char *path, int skippable) {
  if (mp.active)
    movie_stop();
  mp_skippable = skippable;
  movie_set_text(NULL); // no stale subtitles from a previous movie

  memset(&mp, 0, sizeof(mp));
  // init the lock first so the failure path can always tear it down
  mtx_init(&mp.lock, mtx_plain);
  cnd_init(&mp.can_produce);

  if (path && path[0] == '/')
    path++;

  if (avformat_open_input(&mp.fmt, path, NULL, NULL) < 0) {
    debugPrintf("movie: could not open %s\n", path ? path : "(null)");
    mp.fmt = NULL;
    goto fail;
  }
  if (avformat_find_stream_info(mp.fmt, NULL) < 0)
    goto fail;

  mp.vstream = av_find_best_stream(mp.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  mp.astream = av_find_best_stream(mp.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (mp.vstream < 0)
    goto fail;

  AVStream *vs = mp.fmt->streams[mp.vstream];
  if (open_codec(&mp.vctx, vs, 2) < 0)
    goto fail;
  mp.width = mp.vctx->width;
  mp.height = mp.vctx->height;
  mp.video_tb = av_q2d(vs->time_base);
  if (mp.width <= 0 || mp.height <= 0)
    goto fail;

  for (int i = 0; i < NUM_VFRAMES; i++) {
    mp.frames[i].y = malloc((size_t)mp.width * mp.height);
    mp.frames[i].u = malloc((size_t)(mp.width / 2) * (mp.height / 2));
    mp.frames[i].v = malloc((size_t)(mp.width / 2) * (mp.height / 2));
    if (!mp.frames[i].y || !mp.frames[i].u || !mp.frames[i].v)
      goto fail;
  }

  // audio is optional; also requires the game's AL context to exist
  if (mp.astream >= 0 && alcGetCurrentContext() != NULL &&
      open_codec(&mp.actx, mp.fmt->streams[mp.astream], 1) == 0) {
    mp.audio_rate = mp.actx->sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&mp.swr, &out_layout, AV_SAMPLE_FMT_S16, mp.audio_rate,
                            &mp.actx->ch_layout, mp.actx->sample_fmt, mp.audio_rate, 0, NULL) < 0)
      mp.swr = NULL;
#else
    const uint64_t in_layout = mp.actx->channel_layout ?
        mp.actx->channel_layout : av_get_default_channel_layout(mp.actx->channels);
    mp.swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, mp.audio_rate,
                                in_layout, mp.actx->sample_fmt, mp.audio_rate, 0, NULL);
#endif
    if (mp.swr && swr_init(mp.swr) == 0) {
      mp.pcm_acc = malloc((ABUF_SAMPLES + 8192) * 2 * sizeof(int16_t));
      alGenSources(1, &mp.al_source);
      alGenBuffers(NUM_ABUFS, mp.al_buffers);
      alSourcei(mp.al_source, AL_SOURCE_RELATIVE, AL_TRUE);
      alSourcef(mp.al_source, AL_GAIN, 1.0f);
      for (int i = 0; i < NUM_ABUFS; i++)
        mp.al_buf_free[i] = 1;
      mp.has_audio = mp.pcm_acc != NULL;
    } else {
      debugPrintf("movie: audio setup failed, playing video only\n");
      if (mp.swr)
        swr_free(&mp.swr);
      avcodec_free_context(&mp.actx);
    }
  }

  mp.start_tick = armGetSystemTick();

  if (thrd_create(&mp.thread, decoder_main, NULL) != thrd_success)
    goto fail;
  mp.thread_running = 1;
  mp.active = 1;
  debugPrintf("movie: playing %s (%dx%d, audio=%d)\n", path, mp.width, mp.height, mp.has_audio);
  return 1;

fail:
  debugPrintf("movie: failed to start %s\n", path ? path : "(null)");
  mp.stop_req = 1;
  free_session();
  return 0;
}

// on android skipping was handled by the Java video view eating the input;
// the game itself ignores the gamepad while it waits for a movie, so the
// main loop calls this on button presses
void movie_skip(void) {
  if (mp.active && mp_skippable) {
    debugPrintf("movie: skipped by user\n");
    movie_stop();
  }
}

int movie_is_playing(void) {
  if (!mp.active)
    return 0;

  // finished when the decoder hit EOF, the frame ring is drained and the
  // audio queue ran dry (nobody restarts the source after EOF)
  if (mp.decode_eof && mp.frame_count == 0) {
    int audio_done = 1;
    if (mp.has_audio) {
      ALint state = 0;
      alGetSourcei(mp.al_source, AL_SOURCE_STATE, &state);
      audio_done = (state != AL_PLAYING);
    }
    if (audio_done) {
      movie_stop();
      return 0;
    }
  }
  return 1;
}
