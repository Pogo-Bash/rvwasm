/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * A stand-in for the page's WebCodecs VideoDecoder in native tests: a stateful decoder (FFmpeg's libavcodec)
 * fed one chunk at a time, each chunk's timestamp passed through to its frame, as VideoDecoder does. Built
 * for riscv64 against Debian's libavcodec and run under qemu-riscv64 by vdec-native-server.py.
 *
 * In (stdin): {u32 op, u32 a, u32 b, u32 size} + size bytes. op 1 configure (a: codec, 1 H.264 2 HEVC 3 VP8 4 VP9
 * 5 AV1; b: frames of reordering, see below), op 2 decode (a: timestamp, b: flags, 1 key), op 3 flush.
 *
 * Unlike Chrome's decoders (media/gpu/h264_decoder.cc: with max_num_reorder_frames 0 every picture is given
 * as soon as it is decoded, out of order or not), FFmpeg's H.264 decoder drops a picture that comes out of
 * order when it thinks there is no reordering (h264_slice.c, h264_select_output_frame). So for H.264 it is
 * told the stream's real reordering (the server's --reorder, from ffprobe's has_b_frames): it then gives the
 * pictures in display order with that delay, which is the delay the player in the guest has too.
 * Out (stdout), after each op: per frame {u32 timestamp, u32 width, u32 height, u32 0} + NV12 bytes
 * (width x height luma, then interleaved chroma), then {u32 0, 0, 0, u32 status} (status 1 done, 2 error).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

static AVCodecContext *ctx;
static AVPacket *pkt;
static AVFrame *frame;

static int get(void *p, size_t n)
{
	return fread(p, 1, n, stdin) == n ? 0 : -1;
}

static void put(const void *p, size_t n)
{
	fwrite(p, 1, n, stdout);
}

static void done(uint32_t status)
{
	uint32_t h[4] = { 0, 0, 0, status };

	put(h, sizeof(h));
	fflush(stdout);
}

static void frames(void)
{
	while (avcodec_receive_frame(ctx, frame) == 0) {
		uint32_t w = (uint32_t)frame->width, h = (uint32_t)frame->height;
		uint32_t hd[4] = { (uint32_t)frame->pts, w, h, 0 };
		uint8_t *row = malloc(w + 2);

		put(hd, sizeof(hd));
		for (uint32_t y = 0; y < h; y++)
			put(frame->data[0] + (size_t)y * frame->linesize[0], w);
		for (uint32_t y = 0; y < h / 2; y++) {
			for (uint32_t x = 0; x < w / 2; x++) {
				row[2 * x] = frame->data[1][(size_t)y * frame->linesize[1] + x];
				row[2 * x + 1] = frame->data[2][(size_t)y * frame->linesize[2] + x];
			}
			put(row, w & ~1u);
		}
		free(row);
		av_frame_unref(frame);
	}
}

int main(void)
{
	uint32_t h[4];
	uint8_t *data = NULL;
	size_t cap = 0;

	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	while (!get(h, sizeof(h))) {
		if (h[3] > cap) {
			cap = h[3] + AV_INPUT_BUFFER_PADDING_SIZE;
			data = realloc(data, cap);
		}
		if (h[3] && get(data, h[3]))
			break;
		if (h[0] == 1) {
			static const enum AVCodecID ids[] = { 0, AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_VP8,
							      AV_CODEC_ID_VP9, AV_CODEC_ID_AV1 };
			const AVCodec *c = h[1] == 5 ? avcodec_find_decoder_by_name("libdav1d") : NULL;

			if (!c && h[1] < 6)
				c = avcodec_find_decoder(ids[h[1]]);
			if (ctx)
				avcodec_free_context(&ctx);
			ctx = c ? avcodec_alloc_context3(c) : NULL;
			if (ctx) {
				if (h[2])
					ctx->has_b_frames = (int)h[2];
				else
					ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
				ctx->thread_count = 1;
			}
			done(ctx && avcodec_open2(ctx, c, NULL) == 0 ? 1 : 2);
		} else if (h[0] == 2 && ctx) {
			int r;

			memset(data + h[3], 0, AV_INPUT_BUFFER_PADDING_SIZE);
			pkt->data = data;
			pkt->size = (int)h[3];
			pkt->pts = h[1];
			pkt->dts = h[1];
			pkt->flags = (h[2] & 1) ? AV_PKT_FLAG_KEY : 0;
			r = avcodec_send_packet(ctx, pkt);
			frames();
			done(r < 0 ? 2 : 1);
		} else if (h[0] == 3 && ctx) {
			avcodec_send_packet(ctx, NULL);
			frames();
			avcodec_flush_buffers(ctx);
			done(1);
		} else {
			done(2);
		}
	}
	return 0;
}
