#ifndef SPOTLIGHT_H_
#define SPOTLIGHT_H_
#include <confuse.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
#include <time.h>
#include <stdlib.h>

extern const char* const SPOTLIGHT_CONFIG_FILE;
extern cfg_t* C_CONFIG;

extern cfg_opt_t capture_opts[];
extern cfg_opt_t scale_opts[];
extern cfg_opt_t spotlight_opts[];
extern cfg_opt_t audio_opts[];
extern cfg_opt_t audio_device_opts[];
extern cfg_opt_t codec_opts[];
extern cfg_opt_t config_opts[];
extern cfg_opt_t export_opts[];

extern cfg_t *C_CAPTURE_ROOT;
extern cfg_t *C_SCALE_ROOT;
extern cfg_t *C_SPOTLIGHT_ROOT;
extern cfg_t *C_AUDIO_ROOT;
extern cfg_t *C_CODEC_ROOT;
extern cfg_t *C_EXPORT_ROOT;



extern int init_config();
extern int load_config();
extern void free_config();

struct VideoStream;
struct AudioStream;
struct Capture;

typedef struct VideoStream {
	AVStream* stream;
	AVCodecContext* codecContext;
	const AVCodec* codec;

	AVFrame **frameBuffer;
	AVPacket *packet;
	size_t bufferSize;

	struct Capture *root;

	struct SwsContext *swsPixfmtConverter;

	size_t sourceHeight, sourceWidth;
	size_t frameHeight, frameWidth;

	size_t writeIndex;
	size_t frameCount;
	size_t pts;
} VideoStream;

struct AudioDevice;
typedef struct AudioStream {
	AVStream* stream;
	AVCodecContext* codecContext;
	const AVCodec* codec;

	AVFrame **frameBuffer;
	AVFrame *resampleFrame;
	AVPacket *packet;
	struct AudioDevice *device;
	size_t bufferSize;

	struct Capture *root;

	struct SwrContext *resampler;

	size_t writeIndex;
	size_t frameCount;
	size_t pts;
	int numSamples;
} AudioStream;

typedef struct Capture {
	int nb_video_streams;
	VideoStream** video_streams;
	int nb_audio_streams;
	AudioStream** audio_streams;
	AVFormatContext *formatContext;
	size_t windowSize;
	size_t framerate;
} Capture;

extern Capture *alloc_capture();
void flush_capture(Capture*, char*);
extern void free_capture(Capture*);
extern void add_video_stream(Capture*, VideoStream*);
extern void add_audio_stream(Capture*, AudioStream*);
extern char* generate_output_filename();

#include "video.h"
#include "audio.h"

#endif
