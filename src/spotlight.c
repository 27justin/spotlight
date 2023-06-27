#include "spotlight.h"

void free_video_stream(VideoStream*);

cfg_t *C_CONFIG;
cfg_t *C_CAPTURE_ROOT;
cfg_t *C_SCALE_ROOT;
cfg_t *C_SPOTLIGHT_ROOT;
cfg_t *C_AUDIO_ROOT;
cfg_t *C_CODEC_ROOT;
cfg_t *C_EXPORT_ROOT;

const char* const SPOTLIGHT_CONFIG_FILE = "~/.config/spotlight/config.cfg";

cfg_t* SPOTLIGHT_CONFIG;

cfg_opt_t capture_opts[] = {
	CFG_INT("x", 0, CFGF_NONE),
	CFG_INT("y", 0, CFGF_NONE),
	CFG_INT("width", 1920, CFGF_NONE),
	CFG_INT("height", 1080, CFGF_NONE),
	CFG_SEC("scale", scale_opts, CFGF_NONE),
	CFG_END()
};

cfg_opt_t scale_opts[] = {
	CFG_INT("width", 0, CFGF_NONE),
	CFG_INT("height", 0, CFGF_NONE),
	CFG_END()
};

cfg_opt_t spotlight_opts[] = {
	CFG_INT("framerate", 30, CFGF_NONE),
	CFG_INT("window-size", 30, CFGF_NONE),
	CFG_INT("threads", 3, CFGF_NONE),
	CFG_SEC("capture", capture_opts, CFGF_NONE),
	CFG_SEC("audio", audio_opts, CFGF_NONE),
	CFG_END()
};

cfg_opt_t audio_opts[] = {
	CFG_STR("codec", "aac", CFGF_NONE),
	CFG_STR_LIST("merge", "", CFGF_NONE),
	CFG_INT("bitrate", 64000, CFGF_NONE),
	CFG_SEC("device", audio_device_opts, CFGF_TITLE | CFGF_MULTI),
	CFG_END()
};

cfg_opt_t audio_device_opts[] = {
	CFG_STR("name", NULL, CFGF_NONE),
	CFG_STR("channels", "stereo", CFGF_NONE),
	CFG_END()
};

cfg_opt_t codec_opts[] = {
	CFG_STR("name", "libx264", CFGF_NONE),
	CFG_STR("container", "mp4", CFGF_NONE),
	CFG_INT("bitrate", 8000000, CFGF_NONE),
	CFG_SEC("options", NULL, CFGF_KEYSTRVAL | CFGF_IGNORE_UNKNOWN),
	CFG_END()
};

cfg_opt_t export_opts[] = {
	CFG_STR("directory", "~/Videos/", CFGF_NONE),
	CFG_END()
};

cfg_opt_t config_opts[] = {
	CFG_SEC("spotlight", spotlight_opts, CFGF_NONE),
	CFG_SEC("codec", codec_opts, CFGF_NONE),
	CFG_SEC("export", export_opts, CFGF_NONE),
	CFG_END()
};



int init_config() {
	C_CONFIG = cfg_init(config_opts, CFGF_NONE);
	return C_CONFIG != NULL;
}
int load_config() {
	if(cfg_parse(C_CONFIG, SPOTLIGHT_CONFIG_FILE) == CFG_PARSE_ERROR) {
		printf("Error parsing config file: %s\n", SPOTLIGHT_CONFIG_FILE);
		exit(1);
	}
	
	/* Initialize global config sections */
	C_SPOTLIGHT_ROOT = cfg_getsec(C_CONFIG, "spotlight");
	C_CAPTURE_ROOT = cfg_getsec(C_SPOTLIGHT_ROOT, "capture");
	C_SCALE_ROOT = cfg_getsec(C_CAPTURE_ROOT, "scale");
	C_AUDIO_ROOT = cfg_getsec(C_SPOTLIGHT_ROOT, "audio");
	C_CODEC_ROOT = cfg_getsec(C_CONFIG, "codec");
	C_EXPORT_ROOT = cfg_getsec(C_CONFIG, "export");

	return 0;
}

void free_config() {
	cfg_free(SPOTLIGHT_CONFIG);
}

Capture *alloc_capture() {
	Capture *capture = malloc(sizeof(Capture));
	capture->nb_video_streams = 0;
	capture->video_streams = NULL;
	capture->nb_audio_streams = 0;
	capture->audio_streams = NULL;
	
	capture->formatContext = NULL;

	capture->windowSize = cfg_getint(C_SPOTLIGHT_ROOT, "window-size");
	capture->framerate = cfg_getint(C_SPOTLIGHT_ROOT, "framerate");

	avformat_alloc_output_context2(&capture->formatContext, NULL, cfg_getstr(C_CODEC_ROOT, "container"), NULL);
	if(!capture->formatContext) {
		printf("Error allocating output context\n");
		exit(1);
	}

	

	return capture;
}

void free_capture(Capture *capture) {
	int i;
	for(i = 0; i < capture->nb_video_streams; i++) {
		free_video_stream(capture->video_streams[i]);
	}
	free(capture->video_streams);
	for(i = 0; i < capture->nb_audio_streams; i++) {
		free_audio_stream(capture->audio_streams[i]);
	}
	free(capture->audio_streams);
	avio_close(capture->formatContext->pb);
	avformat_free_context(capture->formatContext);
	free(capture);
}

void add_video_stream(Capture *capture, VideoStream *videoStream) {
	printf("[CAPTURE] Adding video stream\n");
	capture->video_streams = realloc(capture->video_streams, sizeof(VideoStream*) * (capture->nb_video_streams + 1));
	capture->video_streams[capture->nb_video_streams] = videoStream;
	capture->nb_video_streams++;
	// Open the AVStream for this object
	if(open_video_stream(capture, videoStream)) {
		fprintf(stderr, "Error opening video stream\n");
		exit(1);
	}
}

void add_audio_stream(Capture *capture, AudioStream *audioStream) {
	capture->audio_streams = realloc(capture->audio_streams, sizeof(VideoStream*) * (capture->nb_audio_streams + 1));
	capture->audio_streams[capture->nb_audio_streams] = audioStream;
	capture->nb_audio_streams++;
	// Open the AVStream for this object
	// TODO: Horrible, for the love of god, fix this.
	// Unlike the VideoStream, we don't know how many buffer AVFrame*s
	// we need for the configured window size, so the alloc_audio_stream() method
	// HAS to open the codec and it's context to get the frame size.
	// Ideally, we want to have the same application flow as the VideoStream,
	// but for now, I'll just leave it at this.
	/*if(open_audio_stream(capture, audioStream)) {
		fprintf(stderr, "Error opening audio stream\n");
		exit(1);
	}*/
}

void flush_capture(Capture* cap, char* file) {
	printf("[CAPTURE] Flushing capture into %s\n", file);
	printf("[CAPTURE] Parameters:\n");
	printf("\t Total streams: %i\n", cap->formatContext->nb_streams);

	printf("[CAPTURE] Opening output file\n");
	avio_open(&cap->formatContext->pb, file, AVIO_FLAG_WRITE);
	printf("[CAPTURE] Writing header to output file\n");
	if(avformat_write_header(cap->formatContext, NULL) < 0) {
		printf("Error writing header\n");
		exit(1);
	}
	// Flushes all streams in the capture
	int i;
	printf("[CAPTURE] Flushing video streams\n");
	for(i = 0; i < cap->nb_video_streams; i++) {
		flush_video_stream(cap->video_streams[i]);
	}
	printf("[CAPTURE] Flushing audio streams\n");
	printf("Flushing %i audio streams\n", cap->nb_audio_streams);
	for(i = 0; i < cap->nb_audio_streams; i++) {
		flush_audio_stream(cap->audio_streams[i]);
	}

	printf("[CAPTURE] Writing trailer to file\n");
	av_write_trailer(cap->formatContext);
	printf("[CAPTURE] Closing output file\n");
	avio_close(cap->formatContext->pb);
	
	// Reset the AVFormatContext as encoding more videos with the same context will cause errors
	// (A bunch of "Application provided invalid, non monotonically increasing dts to muxer in stream 0")
	// As described here: https://stackoverflow.com/questions/53004170/c-ffmpeg-how-to-continue-encoding-after-flushing
	
	printf("Allocating new format context\n");
	avformat_free_context(cap->formatContext);

	AVFormatContext *replacement;
	avformat_alloc_output_context2(&replacement, NULL, cfg_getstr(C_CODEC_ROOT, "container"), NULL);
	cap->formatContext = replacement;

	if(!cap->formatContext) {
		printf("Error allocating output context\n");
		exit(1);
	}

	// Re open streams for video and audio
	for(i = 0; i < cap->nb_video_streams; i++) {
		printf("Reopening video stream %i\n", i);
		if(open_video_stream(cap, cap->video_streams[i])) {
			fprintf(stderr, "Error opening video stream\n");
			exit(1);
		}
	}
	for(i = 0; i < cap->nb_audio_streams; i++) {
		printf("Reopening audio stream %i\n", i);
		if(open_audio_stream(cap, cap->audio_streams[i])) {
			fprintf(stderr, "Error opening audio stream\n");
			exit(1);
		}
	}
	printf(" NEW CONTEXT STREAMS: %i\n", replacement->nb_streams);

}


char* generate_output_filename() {
	time_t rawtime;
	struct tm* timeinfo;
	char* file = malloc(sizeof(char) * 100); // Adjust the size as needed
	char* filename = malloc(sizeof(char) * 100); // Adjust the size as needed
	char* base = cfg_getstr(C_EXPORT_ROOT, "directory");

	if (filename == NULL) {
		fprintf(stderr, "Failed to allocate memory for filename\n");
		return NULL;
	}

	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(filename, 100, "%FT%T", timeinfo);


	sprintf(file, "%s/output-%s.%s", base, filename, cfg_getstr(C_CODEC_ROOT, "container"));
	free(filename);

	return file;
}
