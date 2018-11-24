#include "vine_task.h"
#include "vine_pipe.h"
#include "vine_data.h"
#include "utils/breakdown.h"
#include <stdlib.h>

vine_task_msg_s * vine_task_alloc(vine_pipe_s *vpipe,int ins,int outs)
{
	vine_task_msg_s * task;

	task = (vine_task_msg_s *)vine_object_register( &(vpipe->objs),
													VINE_TYPE_TASK,"Task",
				sizeof(vine_task_msg_s)+sizeof(vine_data*)*(ins+outs));

	if(!task)
		return 0;

	async_completion_init(&(vpipe->async),&(task->done));

	task->in_count = ins;
	task->out_count = outs;
	task->args = 0;

	utils_breakdown_instance_init(&(task->breakdown));

	return task;
}

void vine_task_wait_done(vine_task_msg_s * msg)
{
	async_completion_wait(&(msg->done));
}

void vine_task_mark_done(vine_task_msg_s * msg,vine_task_state_e state)
{
	msg->state = state;
	async_completion_complete(&(msg->done));
}

VINE_OBJ_DTOR_DECL(vine_task_msg_s)
{
	vine_task_msg_s *_task = (vine_task_msg_s *)obj;
	int cnt;
	utils_breakdown_advance(&(_task->breakdown),"TaskFree");

	printf("%s:%p:%s %d\n",__func__,obj,obj->name,vine_object_refs(_task->args));

	if(_task->args)
		vine_object_ref_dec(_task->args);

	for(cnt = 0 ; cnt < _task->in_count+_task->out_count ; cnt++)
		vine_data_free(_task->io[cnt]);

	#ifdef BREAKS_ENABLE
	if(_task->breakdown.stats)
		utils_breakdown_end(&(_task->breakdown));
	#endif

	// This check is necessary as some unit tests leave stats null
	// TODO: Fix this properly
	arch_alloc_free(obj->repo->alloc,obj);

}
