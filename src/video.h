#ifndef VIDEO_H_
#define VIDEO_H_

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <semaphore.h>

#include "spotlight.h"

typedef struct VideoThreadContext VideoThreadContext;
typedef struct VideoThreadOrchestrator {
	float timestamp;
	size_t framerate;

	size_t nb_threads;
	pthread_t* threads;
	VideoThreadContext** contexts;

	VideoStream* stream;
	Display* display;
} VideoThreadOrchestrator;

typedef struct VideoThreadContext {
	int id;
	VideoThreadOrchestrator* sync;
	sem_t active;
	XImage* shmImage;
	XShmSegmentInfo* shmInfo;
	struct SwsContext *formatter;
	volatile int ready; // Flag to indicate whether this thread has set up all thread local variables.
} VideoThreadContext;

VideoStream *default_video(struct Capture*);

void free_video_stream(VideoStream*);
void flush_video_stream(VideoStream*);
void reset_video_stream(VideoStream*);

// TODO: This function name is misleading
int open_video_stream(Capture*, VideoStream*);

void video_encode_ximage(VideoStream*, XImage*, struct SwsContext*);
uint32_t correct_video_drift(VideoStream*, int64_t);





#endif
