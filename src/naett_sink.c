#include "naett_internal.h"

#if __WINDOWS__ || defined(__APPLE__)

#include <stdlib.h>
#include <string.h>
#if __APPLE__
#include <time.h>
#endif

#if __WINDOWS__
typedef CRITICAL_SECTION SinkMutex;
typedef CONDITION_VARIABLE SinkCondition;
#define sinkLock EnterCriticalSection
#define sinkUnlock LeaveCriticalSection
#define sinkSignal WakeAllConditionVariable
#else
typedef pthread_mutex_t SinkMutex;
typedef pthread_cond_t SinkCondition;
#define sinkLock pthread_mutex_lock
#define sinkUnlock pthread_mutex_unlock
#define sinkSignal pthread_cond_broadcast
#endif

static void waitForStream(SinkCondition* condition, SinkMutex* mutex, int timeoutMS) {
#if __WINDOWS__
    SleepConditionVariableCS(condition, mutex, timeoutMS < 0 ? INFINITE : (DWORD)timeoutMS);
#else
    if (timeoutMS < 0) {
        pthread_cond_wait(condition, mutex);
        return;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
        deadline.tv_sec += timeoutMS / 1000;
        deadline.tv_nsec += (timeoutMS % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(condition, mutex, &deadline);
    }
#endif
}

#if __WINDOWS__
static SRWLOCK activityMutex = SRWLOCK_INIT;
static CONDITION_VARIABLE activityChanged = CONDITION_VARIABLE_INIT;
#define activityLock() AcquireSRWLockExclusive(&activityMutex)
#define activityUnlock() ReleaseSRWLockExclusive(&activityMutex)
#else
static pthread_mutex_t activityMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t activityChanged = PTHREAD_COND_INITIALIZER;
#define activityLock() pthread_mutex_lock(&activityMutex)
#define activityUnlock() pthread_mutex_unlock(&activityMutex)
#endif
static uint64_t activityVersion;

static void signalActivity(void) {
    activityLock();
    activityVersion++;
    sinkSignal(&activityChanged);
    activityUnlock();
}

static uint64_t getActivityVersion(void) {
    activityLock();
    uint64_t version = activityVersion;
    activityUnlock();
    return version;
}

static void waitForActivity(uint64_t version, int timeoutMS) {
    activityLock();
    if (version == activityVersion && timeoutMS > 0) {
#if __WINDOWS__
        SleepConditionVariableSRW(&activityChanged, &activityMutex, (DWORD)timeoutMS, 0);
#else
        waitForStream(&activityChanged, &activityMutex, timeoutMS);
#endif
    }
    activityUnlock();
}

struct DownloadSink {
    SinkMutex mutex;
    SinkCondition readable;
    SinkCondition writable;
    unsigned char buffer[65536];
    int offset;
    int size;
    int finished;
    int stopped;
};

static DownloadSink* createSink(void) {
    DownloadSink* sink = calloc(1, sizeof(*sink));
    if (!sink) {
        return NULL;
    }
#if __WINDOWS__
    InitializeCriticalSection(&sink->mutex);
    InitializeConditionVariable(&sink->readable);
    InitializeConditionVariable(&sink->writable);
#else
    if (pthread_mutex_init(&sink->mutex, NULL) != 0) {
        free(sink);
        return NULL;
    }
    if (pthread_cond_init(&sink->readable, NULL) != 0) {
        pthread_mutex_destroy(&sink->mutex);
        free(sink);
        return NULL;
    }
    if (pthread_cond_init(&sink->writable, NULL) != 0) {
        pthread_cond_destroy(&sink->readable);
        pthread_mutex_destroy(&sink->mutex);
        free(sink);
        return NULL;
    }
#endif
    return sink;
}

static void freeSink(DownloadSink* sink) {
    if (sink) {
#if __WINDOWS__
        DeleteCriticalSection(&sink->mutex);
#else
        pthread_cond_destroy(&sink->writable);
        pthread_cond_destroy(&sink->readable);
        pthread_mutex_destroy(&sink->mutex);
#endif
        free(sink);
    }
}

static void finishSink(DownloadSink* sink) {
    sinkLock(&sink->mutex);
    sink->finished = 1;
    sinkSignal(&sink->readable);
    sinkUnlock(&sink->mutex);
}

static void stopSink(DownloadSink* sink) {
    sinkLock(&sink->mutex);
    sink->stopped = 1;
    sink->size = 0;
    sinkSignal(&sink->writable);
    sinkSignal(&sink->readable);
    sinkUnlock(&sink->mutex);
}

static int sinkStopped(DownloadSink* sink) {
    sinkLock(&sink->mutex);
    int stopped = sink->stopped;
    sinkUnlock(&sink->mutex);
    return stopped;
}

static int writeSink(DownloadSink* sink, const void* source, int bytes) {
    const unsigned char* data = source;
    int copied = 0;
    sinkLock(&sink->mutex);
    while (copied < bytes) {
        while (sink->size != 0 && !sink->stopped) {
            waitForStream(&sink->writable, &sink->mutex, -1);
        }
        if (sink->stopped) {
            sinkUnlock(&sink->mutex);
            return -1;
        }
        int count = bytes - copied;
        if (count > (int)sizeof(sink->buffer)) {
            count = (int)sizeof(sink->buffer);
        }
        memcpy(sink->buffer, data + copied, (size_t)count);
        copied += count;
        sink->offset = 0;
        sink->size = count;
        sinkSignal(&sink->readable);
        signalActivity();
    }
    sinkUnlock(&sink->mutex);
    return copied;
}

static int readSink(DownloadSink* sink, void* dest, int capacity, int timeoutMS) {
    sinkLock(&sink->mutex);
    if (sink->size == 0 && !sink->finished && !sink->stopped && timeoutMS > 0) {
        waitForStream(&sink->readable, &sink->mutex, timeoutMS);
    }
    int count = capacity < sink->size ? capacity : sink->size;
    if (count > 0) {
        memcpy(dest, sink->buffer + sink->offset, (size_t)count);
        sink->offset += count;
        sink->size -= count;
        if (sink->size == 0) {
            sinkSignal(&sink->writable);
        }
    }
    sinkUnlock(&sink->mutex);
    return count;
}

struct UploadFeed {
    SinkMutex mutex;
    SinkCondition changed;
    uint64_t attempt;
    void* buffer;
    int capacity;
    int index;
    int supplied;
    int finished;
    int stopped;
};

static UploadFeed* createFeed(void) {
    UploadFeed* feed = calloc(1, sizeof(*feed));
    if (!feed) {
        return NULL;
    }
#if __WINDOWS__
    InitializeCriticalSection(&feed->mutex);
    InitializeConditionVariable(&feed->changed);
#else
    if (pthread_mutex_init(&feed->mutex, NULL) != 0) {
        free(feed);
        return NULL;
    }
    if (pthread_cond_init(&feed->changed, NULL) != 0) {
        pthread_mutex_destroy(&feed->mutex);
        free(feed);
        return NULL;
    }
#endif
    return feed;
}

static void freeFeed(UploadFeed* feed) {
    if (feed) {
#if __WINDOWS__
        DeleteCriticalSection(&feed->mutex);
#else
        pthread_cond_destroy(&feed->changed);
        pthread_mutex_destroy(&feed->mutex);
#endif
        free(feed);
    }
}

typedef enum { FeedRestart, FeedInterrupt, FeedCancel } FeedChange;

static void changeFeed(UploadFeed* feed, FeedChange change) {
    if (feed) {
        sinkLock(&feed->mutex);
        feed->attempt++;
        feed->buffer = NULL;
        feed->finished = change != FeedRestart;
        feed->stopped |= change == FeedCancel;
        sinkSignal(&feed->changed);
        sinkUnlock(&feed->mutex);
    }
}

static void finishFeed(UploadFeed* feed) {
    if (feed) {
        sinkLock(&feed->mutex);
        feed->finished = 1;
        sinkSignal(&feed->changed);
        sinkUnlock(&feed->mutex);
    }
}

static int readFeed(UploadFeed* feed, int index, void* buffer, int capacity) {
    sinkLock(&feed->mutex);
    int count = -1;
    if (!feed->stopped && !feed->finished) {
        uint64_t attempt = feed->attempt;
        feed->buffer = buffer;
        feed->capacity = capacity;
        feed->index = index;
        sinkSignal(&feed->changed);
        signalActivity();
        while (feed->buffer && feed->attempt == attempt) {
            waitForStream(&feed->changed, &feed->mutex, -1);
        }
        if (feed->attempt == attempt) {
            count = feed->supplied;
        }
    }
    sinkUnlock(&feed->mutex);
    return count;
}

static int requestFeed(UploadFeed* feed, int* capacity, uint64_t* attempt, int timeoutMS) {
    sinkLock(&feed->mutex);
    if (!feed->buffer && !feed->finished && !feed->stopped && timeoutMS > 0) {
        waitForStream(&feed->changed, &feed->mutex, timeoutMS);
    }
    int index = feed->finished || feed->stopped ? -1 : 0;
    if (feed->buffer) {
        index = feed->index;
        *capacity = feed->capacity;
        *attempt = feed->attempt;
    }
    sinkUnlock(&feed->mutex);
    return index;
}

static int supplyFeed(UploadFeed* feed, uint64_t attempt, const void* data, int size) {
    sinkLock(&feed->mutex);
    int accepted = feed->buffer && attempt == feed->attempt && size >= 0 && size <= feed->capacity &&
        (size == 0 || data);
    if (accepted) {
        if (size > 0) {
            memcpy(feed->buffer, data, (size_t)size);
        }
        feed->supplied = size;
        feed->buffer = NULL;
        sinkSignal(&feed->changed);
    }
    sinkUnlock(&feed->mutex);
    return accepted;
}

#endif
