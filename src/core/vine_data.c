#include "vine_data.h"
#include "vine_accel.h"
#include "vine_pipe.h"
#include <string.h>
#include <stdlib.h>

//#define printd(...) fprintf(__VA_ARGS__)
#define  printd(...)

#define VDFLAG(DATA,FLAG) (DATA->flags&FLAG)//((size_t*)BUFF-1) same pointer arithmetics//
#define VD_BUFF_OWNER(BUFF) *(vine_data_s**)((char*)BUFF-sizeof(size_t*))

vine_data_s* vine_data_init(vine_pipe_s * vpipe,void * user, size_t size)
{
	vine_data_s *data;

	//dec meta data from shm 
	#ifdef VINE_THROTTLE_DEBUG
	printf("SHM meta init\t");
	#endif
	vine_pipe_size_dec( vpipe, sizeof(vine_data_s) );

	data = (vine_data_s*)vine_object_register(&(vpipe->objs),
											  VINE_TYPE_DATA,
										   "UNUSED",sizeof(vine_data_s),1);

	if(!data)		// GCOV_EXCL_LINE
		return 0;	// GCOV_EXCL_LINE

	data->vpipe = vpipe;
	data->user  = user;
	data->size  = size;
    data->buffer= 0; //init buffer with null
    data->align = 1;
	async_completion_init(&(data->vpipe->async),&(data->ready));

	return data;
}

vine_data_s* vine_data_init_aligned(vine_pipe_s * vpipe,void * user, size_t size,size_t align)
{
	vine_data_s *data;

	if(!align)
		return 0;
	
	//dec meta data from shm 
	#ifdef VINE_THROTTLE_DEBUG
	printf("SHM meta init\t");
	#endif
	vine_pipe_size_dec( vpipe, sizeof(vine_data_s) );

	data = (vine_data_s*)vine_object_register(&(vpipe->objs),
											  VINE_TYPE_DATA,
										   "UNUSED",sizeof(vine_data_s),1);

	if(!data)		// GCOV_EXCL_LINE
		return 0;	// GCOV_EXCL_LINE

	data->vpipe = vpipe;
	data->user = user;
	data->size  = size;
    data->align = align;
	async_completion_init(&(data->vpipe->async),&(data->ready));

	return data;
}

void vine_data_check_flags(vine_data_s * data)
{
	switch(data->flags)
	{
		case NONE_SYNC:
		case USER_SYNC:
		case SHM_SYNC:
		case USER_SYNC|SHM_SYNC:
		case REMT_SYNC:
		case REMT_SYNC|SHM_SYNC:
		case ALL_SYNC:
			return;
		default:	// GCOV_EXCL_START
			fprintf(stderr,"%s(%p): Inconsistent data flags %lu\n",__func__,data,data->flags);
			vine_assert(!"Inconsistent data flags");
			// GCOV_EXCL_STOP
	}
}

void vine_data_memcpy(vine_accel * accel,vine_data_s * dst,vine_data_s * src,int block)
{
	if(dst == src)
		return;

	if(vine_data_size(dst) != vine_data_size(src))
	{
		fprintf(stderr,"%s(%p,%p): Size mismatch (%lu,%lu)\n",__func__,dst,src,vine_data_size(dst),vine_data_size(src));
		vine_assert(!"Size mismatch");
	}
	fprintf(stderr,"%s(%p,%p)[%lu,%lu]\n",__func__,dst,src,dst->flags,src->flags);

	vine_data_sync_from_remote(accel,src,block);

	memcpy(vine_data_deref(dst),vine_data_deref(src),vine_data_size(src));

	vine_data_modified(dst,SHM_SYNC);

	vine_data_sync_to_remote(accel,dst,block);
}

static inline void _set_accel(vine_data_s* data,vine_accel * accel,const char * func)
{
	vine_assert(data);
	vine_assert(accel);	// Must be given a valid accelerator

	if( ! data->accel )	// Unassinged data - assign and increase accel
	{
		vine_object_ref_inc(((vine_object_s*)accel));
		data->accel = accel;
		return;
	}

	if ( data->accel == accel ) // Already assigned to accel - nop
		return;

	if ( ((vine_object_s*)(data->accel))->type == ((vine_object_s*)(accel))->type
		&& ((vine_object_s*)(accel))->type == VINE_TYPE_VIRT_ACCEL // Both of them virtual
	)
	{
		vine_vaccel_s * dvac = (vine_vaccel_s*)(data->accel);
		vine_vaccel_s * avac = (vine_vaccel_s*)(accel);
		if(dvac->phys == avac->phys)
		{	// Both use the same physical accel - just migrate ref counts
			vine_object_ref_dec(((vine_object_s*)dvac));
			vine_object_ref_inc(((vine_object_s*)avac));
			data->accel = accel;
			return;
		}
		else
		{	// Device 2 Device migration - migrate data and ref counts
			fprintf(stderr,"Different device data migration!\n");
		}
	}

	// GCOV_EXCL_START
	// More cases, but for now fail

	fprintf(stderr,"%s():Data migration not implemented(%s:%s,%s:%s)!\n",
			func,
		vine_accel_type_to_str(((vine_vaccel_s*)(data->accel))->type),((vine_object_s*)(data->accel))->name,
			vine_accel_type_to_str(((vine_vaccel_s*)(accel))->type),((vine_object_s*)(accel))->name
	);
	vine_assert(!"No migration possible");
	// GCOV_EXCL_STOP

}

void* vine_data_allocate(vine_data_s* data)
{
	vine_object_repo_s *repo = &(data->vpipe->objs);
	void * buffer;

	//allocate the size of data the aligned and one more pointer for owner
	buffer = arch_alloc_allocate(repo->alloc,data->size + data->align + sizeof(size_t*));
	if(!buffer){
		printf("Could not initiliaze vine_data_s object\n");
        vine_assert(buffer);
    }
	//Clean allocated memory
    memset(buffer,0,data->size + data->align + sizeof(size_t*));

    buffer =(char*)buffer + sizeof(size_t*);//(size_t*)buffer + 1

    //check if data->buffer is aligned if not align it !
    if( ((size_t)buffer) % data->align )
        buffer = buffer + data->align - ((size_t)buffer) % data->align;

    VD_BUFF_OWNER(buffer) = data;

    return buffer;
}

void vine_data_arg_init(vine_data_s* data,vine_accel * accel)
{
    //check errors
	vine_assert(data);
	vine_assert(accel);

	if(!data->buffer)
		data->buffer=vine_data_allocate(data);

	_set_accel(data,accel,__func__);

	async_completion_init(&(data->vpipe->async),&(data->ready));
}

void vine_data_input_init(vine_data_s* data,vine_accel * accel)
{
    //check errors
	vine_assert(data);
	vine_assert(accel);

    vine_object_ref_inc(&(data->obj));

	if(!data->buffer)
		data->buffer=vine_data_allocate(data);

	_set_accel(data,accel,__func__);

	async_completion_init(&(data->vpipe->async),&(data->ready));
}

void vine_data_output_init(vine_data_s* data,vine_accel * accel)
{
    //check errors
	vine_assert(data);
	vine_assert(accel);

	vine_object_ref_inc(&(data->obj));

	if(!data->buffer)
		data->buffer=vine_data_allocate(data);

	_set_accel(data,accel,__func__);

	async_completion_init(&(data->vpipe->async),&(data->ready));
}

void vine_data_output_done(vine_data_s* data)
{
	vine_assert(data);
	// Invalidate on all levels except accelerator memory.
	vine_data_modified(data,REMT_SYNC);
}

size_t vine_data_size(vine_data *data)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = data;
	return vdata->size;
}

void* vine_data_deref(vine_data *data)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;

	if(!vdata->buffer)
		vdata->buffer=vine_data_allocate(data);

	return vdata->buffer;
}

vine_data * vine_data_ref(void * data)
{
	vine_assert(data);

	vine_data_s *vdata = VD_BUFF_OWNER(data);

	// GCOV_EXCL_START
	if(!vdata)
		return 0;

	if(vdata->obj.type != VINE_TYPE_DATA)
		return 0;
	// GCOV_EXCL_STOP

	return vdata;
}

void vine_data_mark_ready(vine_pipe_s *vpipe, vine_data *data)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;
	async_completion_complete(&(vdata->ready));
}

int vine_data_check_ready(vine_pipe_s *vpipe, vine_data *data)
{
	vine_assert(data);
	vine_data_s *vdata;
	int return_val;

	vdata = offset_to_pointer(vine_data_s*, vpipe, data);
	return_val = async_completion_check(&(vdata->ready));

	return return_val;
}

void vine_data_free(vine_data *data)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;
	vine_object_ref_dec(&(vdata->obj));
}

void rs_sync(vine_accel * accel, int sync_dir,const char * func,vine_data_s * data,int block)
{
	vine_assert(data);
	if(data->remote == vine_data_deref(data))	// Remote points to shm buffer
		return;

	void * args[2] = {data,(void*)(size_t)block};

	data->sync_dir = sync_dir;

	vine_accel_type_e type = ((vine_vaccel_s*)accel)->type;
	vine_proc_s * proc = vine_proc_get(type,func);

	if(!vine_proc_get_functor(proc))
		return;

	vine_task_msg_s * task = vine_task_issue(accel,proc,args,sizeof(void*)*2,0,0,0,0);

	if(block)
	{
		vine_task_wait(task);
		vine_task_free(task);
	}
}

/*
 * Send user data to the remote
 */
void vine_data_sync_to_remote(vine_accel * accel,vine_data * data,int block)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;

	vine_data_check_flags(data);	// Ensure flags are consistent

	_set_accel(vdata,accel,__func__);

	switch(vdata->flags)
	{
		case USER_SYNC:	// usr->shm
			if(vdata->user){
				memcpy(vine_data_deref(vdata),vdata->user,vdata->size);
            }
			vdata->flags |= USER_SYNC;
		case USER_SYNC|SHM_SYNC:
		case SHM_SYNC:
			rs_sync(accel,TO_REMOTE,"syncTo",vdata,block);
			vdata->flags |= SHM_SYNC;
		case REMT_SYNC|SHM_SYNC:
		case REMT_SYNC:
			vdata->flags |= REMT_SYNC;
		case ALL_SYNC:
			break;	// All set
		default:	// GCOV_EXCL_START
			fprintf(stderr,"%s(%p) unexpected flags %lu!\n",__func__,data,vdata->flags);
			vine_assert(!"Uxpected flags");
			break;
		case NONE_SYNC:
			fprintf(stderr,"%s(%p) called with uninitialized buffer!\n",__func__,data);
			vine_assert(!"Uninitialized buffer");
			// GCOV_EXCL_STOP
	}

	vine_data_check_flags(data);	// Ensure flags are consistent
}

/*
 * Get remote data to user
 */
void vine_data_sync_from_remote(vine_accel * accel,vine_data * data,int block)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;

	vine_data_check_flags(data);	// Ensure flags are consistent

	_set_accel(vdata,accel,__func__);

	switch(vdata->flags)
	{
		case REMT_SYNC: // rmt->shm
			rs_sync(accel,FROM_REMOTE,"syncFrom",vdata,block);
			vdata->flags |= REMT_SYNC;
		case REMT_SYNC|SHM_SYNC:
		case SHM_SYNC:
			if(vdata->user){
				memcpy(vdata->user,vine_data_deref(vdata),vdata->size);
            }
			vdata->flags |= SHM_SYNC;
		case USER_SYNC|SHM_SYNC:
		case USER_SYNC:	// usr->shm
			vdata->flags |= USER_SYNC;
		case ALL_SYNC:
			break;
		default:	// GCOV_EXCL_START
			fprintf(stderr,"%s(%p) unexpected flags %lu!\n",__func__,data,vdata->flags);
			vine_assert(!"Uninitialized buffer");
			break;
		case NONE_SYNC:
			fprintf(stderr,"%s(%p) called with uninitialized buffer!\n",__func__,data);
			vine_assert(!"Uninitialized buffer");
			// GCOV_EXCL_STOP
	}

	vine_data_check_flags(data);	// Ensure flags are consistent
}

void vine_data_modified(vine_data * data,vine_data_flags_e where)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;
	vdata->flags = where;
}

#undef vine_data_stat

void vine_data_stat(vine_data * data,const char * file,size_t line)
{
	vine_assert(data);
	vine_data_s *vdata;

	vdata = (vine_data_s*)data;

	file += strlen(file);
	while(*file != '/')
		file--;

	int scsum = 0;
	int ucsum = 0;
	int cnt;
	char * bytes = vine_data_deref(data);

	for(cnt = 0 ; cnt < vine_data_size(data) ; cnt++)
	{
		scsum += *bytes;
		bytes++;
	}

	bytes = vdata->user;

	if(bytes)
	{

		for(cnt = 0 ; cnt < vine_data_size(data) ; cnt++)
		{
			ucsum += *bytes;
			bytes++;
		}
	}

	fprintf(stderr,"%s(%p)[%lu]:Flags(%s%s%s%s) %08x %08x ?????? @%lu:%s\n",__func__,vdata,vine_data_size(vdata),
		(vdata->flags&USER_SYNC)?"U":" ",
		(vdata->flags&SHM_SYNC)?"S":" ",
		(vdata->flags&REMT_SYNC)?"R":" ",
		(vdata->flags&FREE)?"F":" ",
		ucsum,
		scsum,
		line,file
	);
}

VINE_OBJ_DTOR_DECL(vine_data_s)
{
	vine_data_s * data = (vine_data_s *)obj;
	int freeMe = 0;
    if(data->remote)
	{
		if(!data->accel)
		{
			fprintf(stderr,"vine_data(%p) dtor called, with dangling remote, with no accel!\n",data);
			vine_assert(!"Orphan dangling remote");
		}
		else
		{

			vine_proc_s * free = vine_proc_get(((vine_vaccel_s*)data->accel)->type,"free");
			vine_task_issue(data->accel,free,&(data->remote),sizeof(data->remote),0,0,0,0);
			#ifdef VINE_THROTTLE_DEBUG
			printf("VACCEL dtr\t");
			#endif
			vine_accel_size_inc(((vine_vaccel_s*)(data->accel))->phys,data->size);
			freeMe = 1;
            vine_object_ref_dec(((vine_object_s*)(data->accel)));
		}
	}
	else{
		if(data->accel)
			vine_object_ref_dec(((vine_object_s*)(data->accel)));
		else
			fprintf(stderr,"vine_data(%p,%s,size:%lu) dtor called, data possibly unused!\n",data,obj->name,vine_data_size(data));
	}

	//free vine_data->buffer from shm
    if( data->buffer != 0 ){
        arch_alloc_free(obj->repo->alloc, ((size_t*)(data->buffer))-1);
		if(freeMe == 1 ){
			#ifdef VINE_THROTTLE_DEBUG
			printf("SHM buffer dtr\t");
			#endif
			vine_pipe_size_inc( data->vpipe, data->size + data->align +sizeof(size_t*) );
		}
	}

	//free meta deta from shm
	#ifdef VINE_THROTTLE_DEBUG
	printf("SHM meta dtr\t");
	#endif
	arch_alloc_free(obj->repo->alloc,data);
	if( data->vpipe != 0 )//check is for vine_object unit test !
		vine_pipe_size_inc(data->vpipe, sizeof(vine_data_s));
}
