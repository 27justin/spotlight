#include "video.h"
#include <math.h>
#include <unistd.h>

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

	printf("Configuring codec with:\n\t%dx%d px\n", frameWidth, frameHeight);


	video->bufferSize = cfg_getint(C_SPOTLIGHT_ROOT, "framerate") * cfg_getint(C_SPOTLIGHT_ROOT, "window-size");
	video->frameBuffer = malloc(sizeof(AVFrame*) * video->bufferSize);
	if(video->frameBuffer == NULL) {
		printf("Error allocating frame buffer\n");
		return NULL;
	}

	printf("Allocating individual frames (&i frames)\n", video->bufferSize);

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

	printf("Allocating packet\n");

	video->packet = av_packet_alloc();
	if(video->packet == NULL) {
		printf("Error allocating packet\n");
		return NULL;
	}
	video->packet->data = NULL;
	video->packet->size = 0;

	printf("Allocating SWScaler\n");

	video->swsPixfmtConverter = sws_getContext(
		sourceWidth,
		sourceHeight,
		AV_PIX_FMT_RGB32,
		frameWidth,
		frameHeight,
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


	if(video->swsPixfmtConverter == NULL) {
		printf("Error creating sws context\n");
		return NULL;
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

	printf("\n");
	printf("Encoding %i frames (total %i)\n", video->bufferSize, video->frameCount);
	do {
		AVFrame *frame = video->frameBuffer[start_index];
		frame->pts = av_rescale_q(n, video->codecContext->time_base, video->stream->time_base);
		frame->pkt_dts = av_rescale_q(n, video->codecContext->time_base, video->stream->time_base);

		//printf("\r[VIDEO] Encoding Frame %i/%i (PTS: %ld)", start_index, video->bufferSize, frame->pts);

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

	//video->pts = av_rescale_q(n + 1, video->codecContext->time_base, video->stream->time_base) + video->pts;

	printf("\n");
}

void free_video_stream(VideoStream *video) {
	// Free all the things
	printf("[VIDEO] Freeing colorspace converter\n");
	if(video->swsPixfmtConverter)
		sws_freeContext(video->swsPixfmtConverter);
	printf("[VIDEO] Freeing packet\n");
	av_packet_free(&video->packet);
	printf("[VIDEO] Freeing codec context\n");
	avcodec_free_context(&video->codecContext);
	// Free all frames inside frameBuffer
	for(int i = 0; i < video->bufferSize; i++) {
		av_frame_free(&video->frameBuffer[i]);
	}
}

void video_encode_ximage(VideoStream *video, XImage *screenContent, struct SwsContext *formatter) {
	// TODO: streamline the parameters and rename this function.
	// the requirement for SwsContext pixfmtScaler was added after adding multi-threading to prevent dropped
	// frames from long sws_scale calls.
	// With the old code (SwsContext allocated by VideoStream*); thus multiple threads using one SwsContext,
	// the sws_scale function would more often than not cause a SEGFAULT, the most likely reason is some internal variables
	// in SwsContext being messed up with multi-threading.
	
	//printf("[VID] Circular Buffer #%d, Frame Count %d\n", video->writeIndex, video->frameCount);
	AVFrame* frame = video->frameBuffer[video->writeIndex];
	av_frame_make_writable(frame);

	// Bump up the writeIndex before re-scaling the frame to make sure that
	// other threads write to the correct address, if we take too long.
	video->writeIndex = (video->writeIndex + 1) % video->bufferSize;
	video->frameCount++;
	
	// Convert XImage to AVFrame using the previously initialized swscaler.
	// This scaler converts both RGB32 to YUV420P, and scales the frame down if it was so configured to be.
	// This code sometimes segfaults, print all relevant memory addresses
	// to make debugging easier.
	/*printf("[VID] Converting XImage to AVFrame\n");
	printf("[VID] XImage: %p\n", screenContent->data);
	printf("[VID] AVFrame: %p\n", frame->data);
	printf("[VID] swsPixfmtConverter: %p\n", formatter);
	printf("[VID] screenContent->bytes_per_line: %d\n", screenContent->bytes_per_line);
	printf("[VID] screenContent->height: %d\n", screenContent->height);
	printf("[VID] frame->linesize: %d\n", frame->linesize[0]);
	printf("[VID] frame->height: %d\n", frame->height);
	printf("[VID] Frame idx: %d, buffer size: %d\n", video->writeIndex, video->bufferSize);*/
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


uint32_t correct_video_drift(VideoStream *video, int64_t drift) {
	// Correct the video drift.
	// This function should only EVER be called when `video_encode_ximage` took longer than the frame time.
	// This function will then correct the drift by duplication (drift / frame time) frames, and sleep for the remaining time.

	// Calculate the number of frames by dividing drift by frame time.
	// The number has to be rounded up, as we can't duplicate half a frame.
	float frameTime = 1000.0 / video->codecContext->framerate.num * video->codecContext->framerate.den;
	int64_t driftedFrames = ceil(drift / frameTime);

	// Duplicate the frames
	int i;
	for(i = 0; i < driftedFrames; i++) {
		// memcpy the frame data from the previous frame
		AVFrame* previousFrame = video->frameBuffer[(video->writeIndex - 1) % video->bufferSize];
		AVFrame* frame = video->frameBuffer[video->writeIndex];
		memcpy(frame->data[0], previousFrame->data[0], frame->linesize[0] * video->codecContext->height);

		video->writeIndex = (video->writeIndex + 1) % video->bufferSize;
		video->frameCount++;
	}

	// Sleep for the remaining time
	int64_t remainingTime = (driftedFrames * frameTime) - drift;
	printf("[VIDEO] Drift: %ld ms (%d correction frames were inserted), Remaining Time: %ld ms\n", drift, driftedFrames, remainingTime);
	usleep(remainingTime * 1000);

	return driftedFrames;
}

int open_video_stream(Capture *capture, VideoStream* vstream) {
	// Free all the things
	if(vstream->codecContext)
		avcodec_free_context(&vstream->codecContext);
	// Open the AVStream for this stream in the capture's AVFormatContext
	printf("[VIDEO] Opening video stream in encoder\n");

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
	printf("[VIDEO] Created new stream\n");
	track->id = capture->formatContext->nb_streams - 1;
	vstream->packet->stream_index = track->id;
	vstream->packet->pts = 0;
	vstream->packet->dts = 0;
	vstream->packet->duration = 0;
	vstream->pts = 0;

	printf("[VIDEO] Allocating codec with ID: %d\n", vstream->codec->id);
	vstream->codecContext = avcodec_alloc_context3(vstream->codec);
	if(vstream->codecContext == NULL) {
		printf("Error allocating codec context\n");
		return 1;
	}

	printf("[VIDEO] Populating codec context\n");

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

	printf("[VIDEO] Copying codec parameters to stream\n");

	if(avcodec_parameters_from_context(track->codecpar, vstream->codecContext) < 0) {
		printf("Failed to copy codec parameters to stream\n");
		return 1;
	}

	printf("[VIDEO] Opening codec\n");
	
	AVDictionary *dict = parse_codec_options();
	if(avcodec_open2(vstream->codecContext, vstream->codec, &dict) < 0) {
		printf("Error opening codec\n");
		return 1;
	}

	return 0;
}

