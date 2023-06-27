#include <stdio.h>
#include <confuse.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <pthread.h>
#include <semaphore.h>

#include "audio.h"
#include "video.h"

#define BILLION 1000000000L

struct Capture *G_CAPTURE = NULL;

volatile int EXIT_FLAG = 0;
volatile int WAIT_FLAG = 0;


void flush_spotlight() {
	EXIT_FLAG = 1;
	printf("Freeing config\n");
	free_config();
	printf("Freeing capture\n");
	free_capture(G_CAPTURE);
	printf("... Done.\n");
	exit(0);
}

void save() {
	// Dump the correct window to the output directory}
	WAIT_FLAG = 1;
	char* file = generate_output_filename();
	flush_capture(G_CAPTURE, file);
	free(file);
	WAIT_FLAG = 0;
}

void audio_thread(AudioStream* stream) {
	printf("Starting audio thread\n");
	while(!EXIT_FLAG) {
		while(WAIT_FLAG == 1);
		audio_encode(stream);
	}
}


// Threading basic run down
// 1. Create N threads, each thread gets assigned an ID and a worker context, the worker context contains a broader thread synchronizer.
// The thread synchronizer shall contain two counting semaphores, one that contains the thread number that shall be active next, and one that contains
// the timestamp of the last thread's starting point.
// 2. The thread now shall wait until the thread number semaphore contains his ID.
// 3. The thread shall wait again, until the current time - timestamp semaphore is bigger than the frame time
// 4.0 The thread shall now set the timestamp semaphore's value to the current time.
// 4.1 The thread shall now advance the thread semaphore by (id + 1 % nb_threads).
// 4.2. The thread shall now encode the frame.
// 5. The thread shall now go back to step 2.

#define TIMESPEC_TO_MS(ts) (((float)(ts).tv_sec * 1000.0f) + ((float)(ts).tv_nsec / 1000000.0f))


typedef struct ThreadContext ThreadContext;

typedef struct ThreadSynchronizer {
	float timestamp;
	size_t framerate;

	size_t nb_threads;
	pthread_t* threads;
	ThreadContext** contexts;

	VideoStream* stream;
	Display* display;
} ThreadSynchronizer;

typedef struct ThreadContext {
	int id;
	ThreadSynchronizer* sync;
	sem_t active;
	XImage* shmImage;
	struct SwsContext *formatter;
} ThreadContext;

void video_thread(ThreadContext* arg) {
	ThreadContext* ctx = arg;
	struct timespec threadTime;

	float frameTime = 1000.0 / ctx->sync->framerate;

	printf("Starting video thread %i\n", ctx->id);
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

	printf("Initializing XShm module for thread %i\n", ctx->id);
	if(!XShmQueryExtension(ctx->sync->display)) {
		printf("XShm extension not supported\n");
		return;
	}

	// Create a XShm instance for the X11 display
	// at coordinates captureRoot->x, captureRoot->y, captureRoot->width, captureRoot->height
	XShmSegmentInfo* xShmInfo = malloc(sizeof(XShmSegmentInfo));

	printf("Creating XImage* on thread %i\n", ctx->id);
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


	printf("Allocating SwsContext for thread #%i\n", ctx->id);
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

	printf("Spinning up loop in thread #%i\n", ctx->id);

	if(ctx->id == 1) WAIT_FLAG = 0;

	float frameTimer = 0.0;
	while(1) {
		// Wait for encoder to finish
		while(WAIT_FLAG == 1);
		//printf("Waiting to be woken up (#%i)\n", ctx->id);
		sem_wait(&ctx->active);
		//printf("Thread %i woke up.\n", ctx->id);
		clock_gettime(CLOCK_MONOTONIC, &threadTime);
		frameTimer = TIMESPEC_TO_MS(threadTime) - ctx->sync->timestamp;
		if(frameTimer < frameTime) {
			//printf("Thread %i sleeping for %f\n", ctx->id, frameTime - frameTimer);
			usleep((frameTime - frameTimer) * 1000);
		}

		// Unlock semaphore for next thread
		clock_gettime(CLOCK_MONOTONIC, &threadTime);
		//printf("Updating timestamp\n");
		ctx->sync->timestamp = TIMESPEC_TO_MS(threadTime);

		//printf("Unlocking next thread\n");
		sem_post(&ctx->sync->contexts[(ctx->id + 1) % ctx->sync->nb_threads]->active);

		//printf("Getting Image vom XSHM\n");
		if (!XShmGetImage(ctx->sync->display, DefaultRootWindow(ctx->sync->display), xImage, 0, 0, AllPlanes)) {
			//printf("Error getting image\n");
		}


		//printf("Encoding image (%i)\n", ctx->sync->encodings);
		video_encode_ximage(ctx->sync->stream, xImage, ctx->formatter);

	}
}


int main(int argc, char** argv) {
	signal(SIGINT, flush_spotlight);
	signal(SIGUSR1, save);

	init_config();
	load_config();

	// Initialize capture
	G_CAPTURE = alloc_capture();

	VideoStream *defaultStream = default_video(G_CAPTURE);
	if(!defaultStream) {
		printf("Couldn't initialize X11 video stream.");
		return 1;
	}
	add_video_stream(G_CAPTURE, defaultStream);

	AudioDevice** devices = NULL;
	size_t numDevices = 0;
	if(C_AUDIO_ROOT) {
		devices = init_pulse(&numDevices);
		if(devices == NULL) {
			printf("Error initializing PulseAudio\n");
			return 1;
		}
		for(int i = 0; i < numDevices; i++) {
			AudioStream* stream = alloc_audio_stream(G_CAPTURE, devices[i]);
			add_audio_stream(G_CAPTURE, stream);
		}
	}


	// Initialize X11 display
	Display* xDisplay = XOpenDisplay(NULL);
	if(xDisplay == NULL) {
		printf("Error opening X11 display\n");
		return 1;
	}

	XMapRaised(xDisplay, DefaultRootWindow(xDisplay));

	int framerate = cfg_getint(C_SPOTLIGHT_ROOT, "framerate");
	int xOffset = cfg_getint(C_CAPTURE_ROOT, "x");
	int yOffset = cfg_getint(C_CAPTURE_ROOT, "y");
	int width = cfg_getint(C_CAPTURE_ROOT, "width");
	int height = cfg_getint(C_CAPTURE_ROOT, "height");

	if(!XShmQueryExtension(xDisplay)) {
		printf("XShm extension not supported\n");
		return 1;
	}

	float frameTime = 1000.0 / framerate;
	float drift = 0.0;


	WAIT_FLAG = 1;

	// Create a new thread for each audio stream
	for(int i = 0; i < G_CAPTURE->nb_audio_streams; i++) {
		// TODO: Make this a function and run in threads
		AudioStream* stream = G_CAPTURE->audio_streams[i];
		pthread_t thread;
		pthread_create(&thread, NULL, audio_thread, stream);
	}

	ThreadSynchronizer* sync = malloc(sizeof(ThreadSynchronizer));
	memset(sync, 0, sizeof(ThreadSynchronizer));
	sync->nb_threads = 4;
	sync->contexts = malloc(sizeof(ThreadContext*) * sync->nb_threads);
	sync->threads = malloc(sizeof(pthread_t) * sync->nb_threads);
	sync->framerate = framerate;
	sync->timestamp = 0;
	sync->stream = defaultStream;
	sync->display = xDisplay;

	for(int i = 0; i < 4; i++) {
		ThreadContext* ctx = malloc(sizeof(ThreadContext));
		memset(ctx, 0, sizeof(ThreadContext));
		ctx->id = i;
		ctx->sync = sync;
		// Initialize semaphore
		sem_init(&ctx->active, 0, i == 0 ? 1 : 0);

		sync->contexts[i] = ctx;
		pthread_create(&sync->threads[i], NULL, video_thread, ctx);
	}
	while(1);
}
