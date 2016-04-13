#include <vine_talk.h>
#include <vine_pipe.h>
#include "arch/alloc.h"
#include "utils/queue.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

static void * shm = 0;
static vine_pipe_s * vpipe;
#define SHM_NAME "test"
/* 128 Mb Shared segment */
#define SHM_SIZE 128*1024*1024
/* 128 slost in ring */
#define RING_SIZE 128

#define MY_ID 0

static void prepare_shm() __attribute__ ((constructor));

void prepare_shm()
{
	int err = 0;
	/* Once we figure configuration we will get the shm size,name dynamically */
	int fd = 0;

	if(vpipe)	/* Already initialized */
		return;

	fd = shm_open(SHM_NAME,O_CREAT|O_RDWR,S_IRWXU);

	if(fd < 0)
	{
		err = __LINE__;
		goto FAIL;
	}

	if (ftruncate(fd, SHM_SIZE))
	{
		err = __LINE__;
		goto FAIL;
	}

	do
	{
		shm = mmap(shm,SHM_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|(shm?MAP_FIXED:0),fd,0);

		if(!shm || shm == MAP_FAILED)
		{
			err = __LINE__;
			goto FAIL;
		}

		vpipe = vine_pipe_init(shm,SHM_SIZE,RING_SIZE);
		shm = vpipe->self;			/* This is where i want to go */

		if(vpipe != vpipe->self)
		{
			printf("Remapping from %p to %p.\n",vpipe,vpipe->self);
			munmap(vpipe,SHM_SIZE);
		}

	}while(shm != vpipe);	/* Not where i want */
	printf("ShmLocation:%p\n",shm);
	printf("ShmSize:%d\n",SHM_SIZE);

	return;

	FAIL:
		printf("prepare_shm Failed on line %d (shm:%p)\n",err,shm);
		exit(0);
}

void destroy_shm() __attribute__ ((destructor));

void destroy_shm()
{
	int last = vine_pipe_exit(vpipe);
	printf("%s",__func__);
	printf("vine_pipe_exit() = %d\n",last);
	if(last)
		if(shm_unlink(SHM_NAME))
			printf("Could not delete \"%s\"\n",SHM_NAME);
}

int vine_accel_list(vine_accel_type_e type,vine_accel *** accels)
{
	vine_accel ** accel_array;	// TODO: Do it dynamically
	int accel_count = utils_list_to_array(&(vpipe->accelerator_list),0);
	if(!accels)	/* Only need the count */
		return accel_count;
	accel_array = malloc(sizeof(vine_accel*)*accel_count);
	utils_list_to_array(&(vpipe->accelerator_list),accel_array);
	*accels = accel_array;
	return accel_count;
}

vine_accel_loc_s vine_accel_location(vine_accel * accel)
{
	vine_accel_loc_s ret;
	/*
	 * TODO: Implement
	 */
	return ret;
}

vine_accel_type_e vine_accel_type(vine_accel * accel)
{
	vine_accel_s * _accel;
	_accel = accel;
	return _accel->type;
}

vine_accel_state_e vine_accel_stat(vine_accel * accel,vine_accel_stats_s * stat)
{
	vine_accel_s * _accel;
	_accel = accel;
	return _accel->state;
}

int vine_accel_acquire(vine_accel * accel)
{
	vine_accel_s * _accel;
	_accel = accel;

	return __sync_bool_compare_and_swap(&(_accel->owner),0,MY_ID);
}

void vine_accel_release(vine_accel * accel)
{
	vine_accel_s * _accel;
	__sync_bool_compare_and_swap(&(_accel->owner),MY_ID,0);
}

vine_proc * vine_proc_register(vine_accel_type_e type,const char * func_name,const void * func_bytes,size_t func_bytes_size)
{
	vine_proc_s * proc;

	proc = vine_proc_find_proc(vpipe,func_name,type);

	if(!proc)
	{/* Proc has not been declared */
		proc = arch_alloc_allocate(vpipe->allocator,vine_proc_calc_size(func_name,func_bytes_size));
		proc = vine_proc_init(proc,func_name,type,func_bytes,func_bytes_size);
		vine_pipe_register_proc(vpipe,proc);
	}
	else
	{
		/* Proc has been re-declared */
		if( !vine_proc_code_match(proc,func_bytes,func_bytes_size) )
			return 0;	/* Different than before */
	}
	vine_proc_mod_users(proc,+1);	/* Increase user count */
	return proc;
}

vine_proc * vine_proc_get(vine_accel_type_e type,const char * func_name)
{
	vine_proc_s * proc = vine_proc_find_proc(vpipe,func_name,type);
	if(proc)
		vine_proc_mod_users(proc,+1);	/* Increase user count */
	return proc;
}

int vine_proc_put(vine_proc * func)
{
	vine_proc_s * proc = func;
	return vine_proc_mod_users(proc,-1);	/* Decrease user count */
}

vine_data * vine_data_alloc(size_t size,vine_data_alloc_place_e place)
{
	void * mem;
	mem = arch_alloc_allocate(vpipe->allocator,size+sizeof(vine_data_s));
	return pointer_to_offset(vine_data*,vpipe,vine_data_init(vpipe,mem,size,place));
}

size_t vine_data_size(vine_data * data)
{
	vine_data_s * vdata;
	vdata = offset_to_pointer(vine_data_s*,vpipe,data);
	return vdata->size;
}

void * vine_data_deref(vine_data * data)
{
	vine_data_s * vdata;
	vdata = offset_to_pointer(vine_data_s*,vpipe,data);
	if(!vdata->place&HostOnly)
		return 0;
	return (void*)(vdata+1);
}

void vine_data_mark_ready(vine_data * data)
{
	vine_data_s * vdata;
	vdata = offset_to_pointer(vine_data_s*,vpipe,data);
	__sync_bool_compare_and_swap(&(vdata->ready),0,1);
}

void vine_data_free(vine_data * data)
{
	vine_data_s * vdata;
	vdata = offset_to_pointer(vine_data_s*,vpipe,data);
	arch_alloc_free(vpipe->allocator,vdata);
}

vine_task * vine_task_issue(vine_accel * accel,vine_proc * proc,vine_data * args,size_t in_count,vine_data ** input,size_t out_count,vine_data ** output)
{
	vine_task_msg_s * task = arch_alloc_allocate(vpipe->allocator,sizeof(vine_task_msg_s)+sizeof(vine_data*)*(in_count+out_count));
	vine_data ** dest = task->io;
	int cnt;
	/*
	 vine_accel * accel;
	 vine_proc * proc;
	 vine_data * args;
	 int in_count;
	 int out_count;
	 vine_data * io;
	 */
	task->accel = accel;
	task->proc = proc;
	task->args = args;
	task->in_count = in_count;
	for(cnt = 0 ; cnt < in_count ;cnt++)
		*(dest++) = *(input++);
	task->out_count = out_count;
	for(cnt = 0 ; cnt < out_count ;cnt++)
		*(dest++) = *(output++);
	/* Push it or spin */
	while(!utils_queue_push(vpipe->queue,pointer_to_offset(void*,vpipe,task)));
	task->state = task_issued;
	return task;
}

vine_task_state_e vine_task_stat(vine_task * task,vine_task_stats_s * stats)
{
	return task_failed;
}

vine_task_state_e vine_task_wait(vine_task * task)
{
	vine_task_msg_s * _task = task;
	int start = _task->in_count;
	int end = start + _task->out_count;
	int out;
	vine_data_s * vdata;
	for(out = start;out < end ; out++)
	{
		vdata = offset_to_pointer(vine_data_s*,vpipe,_task->io[out]);
		while(!vdata->ready);
	}
	return _task->state;
}
