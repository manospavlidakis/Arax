#ifndef ASYNC_HEADER
#define ASYNC_HEADER
#include "core/vine_object.h"
#include "pthread.h"
#include <semaphore.h>

typedef struct
{
	#ifndef __cplusplus
	char padd;
	#endif
}async_meta_s;

typedef struct
{
	pthread_mutex_t mutex;
	pthread_mutexattr_t attr;
}
async_completion_s;

typedef struct
{
	sem_t sem;
}async_semaphore_s;
#include "async_api.h"
#endif
