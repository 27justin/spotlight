#include "video.h"
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/shm.h>
#include <sys/ipc.h>



extern Capture *G_CAPTURE;

static void video_worker(VideoThreadContext* ctx);


AVDictionary* parse_codec_options() {
	// Take the options string from the config file and parse it into a dictionary
	// The options string looks like this [name=value,?]*
	
	AVDictionary* options = NULL;
	cfg_t* optionsSection = cfg_getsec(C_CODEC_ROOT, "options");
	if(optionsSection == NULL) {
		return options;
	}
	int i;
	for(i = 0; i < cfg_num(optionsSection); i++) {
		cfg_opt_t *opt = cfg_getnopt(optionsSection, i);
		char* name = cfg_opt_name(opt);
		char* value = cfg_opt_getstr(opt);
		av_dict_set(&options, name, value, 0);
	}
	return options;
}

// Create a video stream from the spotlight config
VideoStream *default_video(Capture *root) {
	if(C_CONFIG == NULL) return NULL;
	VideoStream *video = malloc(sizeof(VideoStream));
	memset(video, 0, sizeof(VideoStream));
	video->root = root;

	int frameHeight, frameWidth, sourceHeight, sourceWidth;
	frameHeight = sourceHeight = cfg_getint(C_CAPTURE_ROOT, "height");
	frameWidth = sourceWidth = cfg_getint(C_CAPTURE_ROOT, "width");
	// Check whether the `scale` block ist set.
	// If so, use those width and heights,
	// otherwise use the capture width and height
	if(cfg_size(C_CAPTURE_ROOT, "scale") > 0) {
		if(cfg_getint(C_SCALE_ROOT, "width") && cfg_getint(C_SCALE_ROOT, "height")) {
			frameHeight = cfg_getint(C_SCALE_ROOT, "height");
			frameWidth = cfg_getint(C_SCALE_ROOT, "width");
		}
	}

	video->sourceHeight = sourceHeight;
	video->sourceWidth = sourceWidth;
	video->frameHeight = frameHeight;
	video->frameWidth = frameWidth;

	video->bufferSize = cfg_getint(C_SPOTLIGHT_ROOT, "framerate") * cfg_getint(C_SPOTLIGHT_ROOT, "window-size");
	video->frameBuffer = malloc(sizeof(AVFrame*) * video->bufferSize);
	if(video->frameBuffer == NULL) {
		printf("Error allocating frame buffer\n");
		return NULL;
	}


	// Allocate AVFrame's inside frame buffer
	for(int i = 0; i < video->bufferSize; i++) {
		video->frameBuffer[i] = av_frame_alloc();
		if(video->frameBuffer[i] == NULL) {
			printf("Error allocating frame %d\n", i);
			return NULL;
		}
		video->frameBuffer[i]->format = AV_PIX_FMT_YUV420P;
		video->frameBuffer[i]->width = frameWidth;
		video->frameBuffer[i]->height = frameHeight;
		av_frame_get_buffer(video->frameBuffer[i], 0);
	}


	video->packet = av_packet_alloc();
	if(video->packet == NULL) {
		printf("Error allocating packet\n");
		return NULL;
	}
	video->packet->data = NULL;
	video->packet->size = 0;

	// Initialize multi-threading for this context
	VideoThreadOrchestrator *orch = (VideoThreadOrchestrator*) malloc(sizeof(VideoThreadOrchestrator));
	video->orchestrator = orch;
	int threads = cfg_getint(C_SPOTLIGHT_ROOT, "threads");
	orch->nb_threads = threads;
	orch->contexts = malloc(sizeof(VideoThreadContext*) * threads);
	orch->threads = malloc(sizeof(pthread_t*) * threads);
	orch->timestamp = 0.0f;
	orch->framerate = cfg_getint(C_SPOTLIGHT_ROOT, "framerate");
	orch->stream = video;


	Display* display = XOpenDisplay(NULL);
	orch->display = display;

	XMapRaised(display, DefaultRootWindow(display));

	int i;
	for(i = 0; i < threads; ++i) {
		VideoThreadContext *ctx = malloc(sizeof(VideoThreadContext));
		memset(ctx, 0, sizeof(VideoThreadContext));
		ctx->id = i;
		ctx->sync = orch;
		ctx->ready = 0;
		sem_init(&ctx->active, 0, i == 0 ? 1 : 0);
		orch->contexts[i] = ctx;
		pthread_create(&orch->threads[i], NULL, video_worker, ctx);
	}

	return video;
}

void flush_video_stream(VideoStream *video) {
	int start_index;
	int encoding = 1;

	if (video->frameCount > video->bufferSize) {
		start_index = video->writeIndex;
	} else {
		start_index = 0;
	}
	int n = 0;
	int ret;

	do {
		AVFrame *frame = video->frameBuffer[start_index];
		frame->pts = av_rescale_q(n, video->codecContext->time_base, video->stream->time_base);
		frame->pkt_dts = av_rescale_q(n, video->codecContext->time_base, video->stream->time_base);

		printf("\r[VIDEO] Encoding Frame %i/%i (PTS: %ld)", start_index, video->bufferSize, frame->pts);

		ret = avcodec_send_frame(video->codecContext, frame);
		if (ret < 0) {
			printf("Error sending frame for encoding\n");
			return;
		}
		while (ret >= 0) {
			ret = avcodec_receive_packet(video->codecContext, video->packet);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				goto outer;
			} else if (ret < 0) {
				printf("Error during encoding\n");
				return;
			}

			// Calculate packet duration
			video->packet->duration = av_rescale_q(video->packet->duration, video->codecContext->time_base, video->stream->time_base);
			// Write packet to file
			av_interleaved_write_frame(video->root->formatContext, video->packet);
			av_packet_unref(video->packet);
		}

outer:
		++n;
		if (n == video->bufferSize) {
			encoding = 0;
		}
		start_index = (start_index + 1) % video->bufferSize;
	}while (encoding);

}

// TODO: Debug this function, as of now it isn't really used as the binary should
// never really exit, except with SIGINT, in which case we don't need to free.
void free_video_stream(VideoStream *video) {
	// Kill all threads
	for(int i = 0; i < video->orchestrator->nb_threads; i++) {
		VideoThreadContext *ctx = video->orchestrator->contexts[i];
		pthread_kill(video->orchestrator->threads[i], SIGINT);
		// Free XShm stuff
		if(ctx->shmInfo->shmaddr != NULL) {
			XShmDetach(ctx->sync->display, ctx->shmInfo);
			shmdt(ctx->shmInfo->shmaddr);
			shmctl(ctx->shmInfo->shmid, IPC_RMID, 0);
		}
		free(ctx);
	}
	// Free XDisplay
	XCloseDisplay(video->orchestrator->display);
	free(video->orchestrator->contexts);
	free(video->orchestrator->threads);
	free(video->orchestrator);
	
	// Free all the things
	av_packet_free(&video->packet);
	avcodec_free_context(&video->codecContext);
	// Free all frames inside frameBuffer
	for(int i = 0; i < video->bufferSize; i++) {
		av_frame_free(&video->frameBuffer[i]);
	}
	free(video->frameBuffer);
}

void video_encode_ximage(VideoStream *video, XImage *screenContent, struct SwsContext *formatter) {
	// TODO: streamline the parameters and rename this function.
	// the requirement for SwsContext pixfmtScaler was added after adding multi-threading to prevent dropped
	// frames from long sws_scale calls.
	// With the old code (SwsContext allocated by VideoStream*); thus multiple threads using one SwsContext,
	// the sws_scale function would more often than not cause a SEGFAULT, the most likely reason is some internal variables
	// in SwsContext being messed up with multi-threading.
	
	AVFrame* frame = video->frameBuffer[video->writeIndex];
	av_frame_make_writable(frame);

	// Bump up the writeIndex before re-scaling the frame to make sure that
	// other threads write to the correct address, if we take too long.
	video->writeIndex = (video->writeIndex + 1) % video->bufferSize;
	video->frameCount++;
	
	// Convert XImage to AVFrame using the previously initialized swscaler.
	// This scaler converts both RGB32 to YUV420P, and scales the frame down if it was so configured to be.
	sws_scale(
		formatter,
		(const uint8_t * const *) &screenContent->data,
		&screenContent->bytes_per_line,
		0,
		screenContent->height,
		frame->data,
		frame->linesize
	);

}


int open_video_stream(Capture *capture, VideoStream* vstream) {
	// Free all the things
	if(vstream->codecContext)
		avcodec_free_context(&vstream->codecContext);
	// Open the AVStream for this stream in the capture's AVFormatContext

	const char* codecName = cfg_getstr(C_CODEC_ROOT, "name");
	int bitrate = cfg_getint(C_CODEC_ROOT, "bitrate");

	vstream->codec = avcodec_find_encoder_by_name(codecName);
	if(vstream->codec == NULL) {
		printf("Error finding codec %s\n", codecName);
		return 1;
	}

	AVStream *track = avformat_new_stream(capture->formatContext, NULL);
	if (!track) {
		printf("Could not allocate stream\n");
		return 1;
	}
	track->id = capture->formatContext->nb_streams - 1;
	vstream->packet->stream_index = track->id;
	vstream->packet->pts = 0;
	vstream->packet->dts = 0;
	vstream->packet->duration = 0;
	vstream->pts = 0;

	vstream->codecContext = avcodec_alloc_context3(vstream->codec);
	if(vstream->codecContext == NULL) {
		printf("Error allocating codec context\n");
		return 1;
	}


	vstream->codecContext->bit_rate = bitrate;
	vstream->codecContext->width = vstream->frameWidth;
	vstream->codecContext->height = vstream->frameHeight;
	// TODO: Preferably, the required variables for this operation should be set in the struct,
	// and not dynamically retrieved from the config.
	vstream->codecContext->time_base = (AVRational){1, cfg_getint(C_SPOTLIGHT_ROOT, "framerate")};
	vstream->codecContext->framerate = (AVRational){cfg_getint(C_SPOTLIGHT_ROOT, "framerate"), 1};
	vstream->codecContext->gop_size = 10;
	vstream->codecContext->max_b_frames = 1;
	vstream->codecContext->pix_fmt = AV_PIX_FMT_YUV420P;

	// Initialize track's codec parameters
	AVCodecParameters *codecParams = track->codecpar;
	codecParams->codec_id = vstream->codec->id;
	codecParams->bit_rate = vstream->codecContext->bit_rate;
	codecParams->codec_type = AVMEDIA_TYPE_VIDEO;
	codecParams->width = vstream->frameWidth;
	codecParams->height = vstream->frameHeight;

	vstream->stream = track;

	if(avcodec_parameters_from_context(track->codecpar, vstream->codecContext) < 0) {
		printf("Failed to copy codec parameters to stream\n");
		return 1;
	}

	AVDictionary *dict = parse_codec_options();
	if(avcodec_open2(vstream->codecContext, vstream->codec, &dict) < 0) {
		printf("Error opening codec\n");
		return 1;
	}

	return 0;
}


#define TIMESPEC_TO_MS(ts) (((float)(ts).tv_sec * 1000.0f) + ((float)(ts).tv_nsec / 1000000.0f))

static void video_worker(VideoThreadContext *ctx) {
	struct timespec threadTime;

	float frameTime = 1000.0 / ctx->sync->framerate;

	// Initialize XShm module for this thread.
	// In the past, multiple threads used a single XImage for capturing, 
	// however this was subject to race conditions in specific contexts.
	// The threads are synchronized to always take a screenshot at the frame time
	// for 30 fps, this would be 33.3ms, if a thread spends more than frame-time on `video_encode_ximage`
	// then the next thread is already overwriting the image data the previous thread is still working on.
	// For my point of view, this is a suboptimal, but the only feasible solution.
	// The resource overhead should be somewhere along the lines of (width * height * 4) * nb_threads + SHM structs
	// which is a lot, but acceptable for now.
	
	/* --------------------- INITIALIZE MIT-SHM MODULE --------------------- */
	int framerate = cfg_getint(C_SPOTLIGHT_ROOT, "framerate");
	int xOffset = cfg_getint(C_CAPTURE_ROOT, "x");
	int yOffset = cfg_getint(C_CAPTURE_ROOT, "y");
	int width = cfg_getint(C_CAPTURE_ROOT, "width");
	int height = cfg_getint(C_CAPTURE_ROOT, "height");

	if(!XShmQueryExtension(ctx->sync->display)) {
		printf("XShm extension not supported\n");
		return;
	}

	// Create a XShm instance for the X11 display
	// at coordinates captureRoot->x, captureRoot->y, captureRoot->width, captureRoot->height
	XShmSegmentInfo* xShmInfo = malloc(sizeof(XShmSegmentInfo));

	XImage* xImage = XShmCreateImage(
		ctx->sync->display,
		XDefaultVisual(ctx->sync->display, XDefaultScreen(ctx->sync->display)),
		XDefaultDepth(ctx->sync->display, XDefaultScreen(ctx->sync->display)),
		ZPixmap,
		NULL,
		xShmInfo,
		width,
		height);

	xShmInfo->shmid = shmget(IPC_PRIVATE, xImage->bytes_per_line * xImage->height, IPC_CREAT | 0600);
	if(xShmInfo->shmid == -1) {
		printf("Error creating shared memory segment\n");
		return;
	}

	xImage->data = shmat(xShmInfo->shmid, 0, 0);
	xShmInfo->shmaddr = xImage->data;

	xShmInfo->readOnly = 0;
	if(!XShmAttach(ctx->sync->display, xShmInfo)) {
		printf("Error attaching shared memory segment\n");
		return;
	}

	ctx->shmInfo = xShmInfo;


	ctx->formatter = sws_getContext(
		ctx->sync->stream->sourceWidth,
		ctx->sync->stream->sourceHeight,
		AV_PIX_FMT_RGB32,
		ctx->sync->stream->frameWidth,
		ctx->sync->stream->frameHeight,
		AV_PIX_FMT_YUV420P,
		// TODO: User should be able to set the scaling algorithm
		// 	     in the config file
		SWS_FAST_BILINEAR, // ~21ms
		//SWS_SINC, // ~40ms
		//SWS_LANCZOS, // ~30ms
		// SWS_SPLINE, // ~31ms
		NULL,
		NULL,
		NULL
	);

	ctx->ready = 1;

	float frameTimer = 0.0;
	while(1) {
		// Wait for encoder to finish
		while(G_CAPTURE->pause == 1);
		sem_wait(&ctx->active);
		clock_gettime(CLOCK_MONOTONIC, &threadTime);
		frameTimer = TIMESPEC_TO_MS(threadTime) - ctx->sync->timestamp;
		if(frameTimer < frameTime) {
			usleep((frameTime - frameTimer) * 1000);
		}

		// Unlock semaphore for next thread
		clock_gettime(CLOCK_MONOTONIC, &threadTime);
		ctx->sync->timestamp = TIMESPEC_TO_MS(threadTime);

		sem_post(&ctx->sync->contexts[(ctx->id + 1) % ctx->sync->nb_threads]->active);

		if (!XShmGetImage(ctx->sync->display, DefaultRootWindow(ctx->sync->display), xImage, 0, 0, AllPlanes)) {
		}


		video_encode_ximage(ctx->sync->stream, xImage, ctx->formatter);

	}

}

