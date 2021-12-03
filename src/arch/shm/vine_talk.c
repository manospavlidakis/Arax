#include <vine_talk.h>
#include <vine_pipe.h>
#include "core/vine_data.h"
#include "utils/config.h"
#include "utils/system.h"
#include "utils/timer.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

const char *vine_talk_version = "VT_VERSION " VINE_TALK_GIT_REV " - " VINE_TALK_GIT_BRANCH;

struct
{
    vine_pipe_s *     vpipe;
    char              shm_file[1024];
    uint64_t          threads;
    uint64_t          instance_uid;
    uint64_t          task_uid;
    volatile uint64_t initialized;
    size_t            initialy_available;
    char *            config_path;
    int               fd;
} vine_state =
{ (void *) CONF_VINE_MMAP_BASE, { '\0' }, 0, 0, 0, 0, 0, NULL };

#define vine_pipe_get() vine_state.vpipe

#define GO_FAIL(MSG)    ({ err = __LINE__; err_msg = MSG; goto FAIL; })

vine_pipe_s* _vine_talk_init(int wait_controller)
{
    vine_pipe_s *shm_addr = 0;
    int err             = 0;
    size_t shm_size     = 0;
    size_t shm_off      = 0;
    int shm_trunc       = 0;
    int shm_ivshmem     = 0;
    int enforce_version = 0;
    int mmap_prot       = PROT_READ | PROT_WRITE | PROT_EXEC;
    int mmap_flags      = MAP_SHARED;
    const char *err_msg = "No Error Set";

    #ifdef MMAP_POPULATE
    mmap_flags |= MAP_POPULATE;
    #endif

    if (__sync_fetch_and_add(&(vine_state.threads), 1) != 0) { // I am not the first but stuff might not yet be initialized
        while (!vine_state.initialized);                       // wait for initialization
        goto GOOD;
    }

    vine_state.config_path = utils_config_alloc_path(VINE_CONFIG_FILE);

    printf("Config:%s\n", VINE_CONFIG_FILE);

    /* Required Confguration Keys */
    if (!utils_config_get_str(vine_state.config_path, "shm_file", vine_state.shm_file, 1024, 0) )
        GO_FAIL("No shm_file set in config");

    /* Default /4 of system memory*/
    shm_size = system_total_memory() / 4;

    utils_config_get_size(vine_state.config_path, "shm_size", &shm_size, shm_size);

    if (!shm_size || shm_size > system_total_memory() )
        GO_FAIL("shm_size exceeds system memory");

    /* Optional Confguration Keys */
    utils_config_get_size(vine_state.config_path, "shm_off", &shm_off, 0);
    utils_config_get_bool(vine_state.config_path, "shm_trunc", &shm_trunc, 1);
    utils_config_get_bool(vine_state.config_path, "shm_ivshmem", &shm_ivshmem, 0);
    utils_config_get_bool(vine_state.config_path, "enforce_version", &enforce_version, 1);

    if (vine_state.shm_file[0] == '/')
        vine_state.fd = open(vine_state.shm_file, O_CREAT | O_RDWR, 0644);
    else
        vine_state.fd = shm_open(vine_state.shm_file, O_CREAT | O_RDWR, S_IRWXU);

    if (vine_state.fd < 0)
        GO_FAIL("Could not open shm_file");

    if (shm_ivshmem) {
        shm_off  += 4096; /* Skip register section */
        shm_trunc = 0;    /* Don't truncate ivshm  device */
    }

    if (shm_trunc) {                                             /* If shm_trunc */
        if (system_file_size(vine_state.shm_file) != shm_size) { /* If not the correct size */
            if (ftruncate(vine_state.fd, shm_size) )
                GO_FAIL("Could not truncate shm_file");
        }
    }

    /* First mmap, probably not where we want */
    vine_state.vpipe = mmap(vine_state.vpipe, shm_size, mmap_prot, mmap_flags,
        vine_state.fd, shm_off);

    if (!vine_state.vpipe || vine_state.vpipe == MAP_FAILED)
        GO_FAIL("Could not first mmap shm_file");

    shm_addr = vine_pipe_mmap_address(vine_state.vpipe);

    if (vine_pipe_have_to_mmap(vine_state.vpipe, system_process_id())) {
        if (shm_addr != vine_state.vpipe) {
            munmap(vine_state.vpipe, vine_state.vpipe->shm_size); // unmap misplaced map.

            vine_state.vpipe = mmap(shm_addr, shm_size, mmap_prot, mmap_flags,
                vine_state.fd, shm_off);
        }
    } else { // Proccess was already mmaped (although we forgot about it :-S
        vine_state.vpipe = shm_addr;
    }

    if (!vine_state.vpipe || vine_state.vpipe == MAP_FAILED || vine_state.vpipe != shm_addr)
        GO_FAIL("Could not mmap shm_file in proper address");

    vine_state.vpipe = vine_pipe_init(vine_state.vpipe, shm_size, enforce_version);

    vine_state.initialy_available = vine_pipe_get_available_size(vine_state.vpipe);

    if (!vine_state.vpipe)
        GO_FAIL("Could not initialize vine_pipe");

    async_meta_init_always(&(vine_state.vpipe->async) );
    printf("ShmFile:%s\n", vine_state.shm_file);
    printf("ShmLocation:%p\n", vine_state.vpipe);
    printf("ShmSize:%zu\n", vine_state.vpipe->shm_size);
    vine_state.instance_uid = __sync_fetch_and_add(&(vine_state.vpipe->last_uid), 1);
    printf("InstanceUID:%zu\n", vine_state.instance_uid);
    vine_state.initialized = 1;
GOOD:
    if (wait_controller) {
        async_condition_lock(&(vine_state.vpipe->cntrl_ready_cond));
        while (vine_state.vpipe->cntrl_ready == 0)
            async_condition_wait(&(vine_state.vpipe->cntrl_ready_cond));
        async_condition_unlock(&(vine_state.vpipe->cntrl_ready_cond));
    }
    return vine_state.vpipe;

FAIL:
    printf("%c[31mprepare_vine_talk Failed on line %d (conf:%s,file:%s,shm:%p)\n\
			Why:%s%c[0m\n", 27, err, VINE_CONFIG_FILE, vine_state.shm_file,
      vine_state.vpipe, err_msg, 27);
    munmap(vine_state.vpipe, vine_state.vpipe->shm_size);
    exit(1);
} /* vine_task_init */

vine_pipe_s* vine_talk_init()
{
    // All applications will wait for the controller/server initialization.
    return _vine_talk_init(1);
}

vine_pipe_s* vine_talk_controller_init_start()
{
    return _vine_talk_init(0);
}

void vine_talk_controller_init_done()
{
    vine_state.vpipe->cntrl_ready = 1;
    async_condition_notify(&(vine_state.vpipe->cntrl_ready_cond));
}

#undef GO_FAIL

uint64_t vine_talk_instance_uid()
{
    return vine_state.instance_uid;
}

void vine_talk_exit()
{
    int last;

    if (vine_state.vpipe) {
        if (__sync_fetch_and_add(&(vine_state.threads), -1) == 1) { // Last thread of process
            last = vine_pipe_exit(vine_state.vpipe);

            if (last) {
                size_t available = vine_pipe_get_available_size(vine_state.vpipe);
                vine_assert(available == vine_state.initialy_available);

                if (available != vine_state.initialy_available) {
                    printf("\033[1;31mERROR : shm LEAK !!\n\033[0m");
                }
            }

            vine_pipe_mark_unmap(vine_state.vpipe, system_process_id());
            munmap(vine_state.vpipe, vine_state.vpipe->shm_size);
            vine_state.vpipe = 0;

            utils_config_free_path(vine_state.config_path);
            printf("vine_pipe_exit() = %d\n", last);
            close(vine_state.fd);
            if (last) {
                if (!vine_talk_clean() )
                    printf("Could not delete \"%s\"\n", vine_state.shm_file);
            }
        }
    } else {
        fprintf(stderr,
          "WARNING:vine_talk_exit() called with no matching\
		call to vine_talk_init()!\n");
    }
} /* vine_talk_exit */

int vine_talk_clean()
{
    char shm_file[1024];

    char *config_path = utils_config_alloc_path(VINE_CONFIG_FILE);

    if (!utils_config_get_str(config_path, "shm_file", shm_file, 1024, 0) )
        vine_assert(!"No shm_file set in config");

    int ret = shm_unlink(shm_file);

    utils_config_free_path(config_path);

    return ret == 0;
}

void vine_accel_set_physical(vine_accel *vaccel, vine_accel *phys)
{
    vine_assert(phys);
    vine_assert(vaccel);
    vine_vaccel_s *acl = (vine_vaccel_s *) vaccel;

    vine_assert(acl);
    vine_accel_add_vaccel(phys, acl);
}

void vine_accel_list_free_pre_locked(vine_accel **accels);

int vine_accel_list(vine_accel_type_e type, int physical, vine_accel ***accels)
{
    vine_pipe_s *vpipe;
    utils_list_node_s *itr;
    utils_list_s *acc_list;

    vine_accel_s **acl = 0;
    int accel_count    = 0;
    vine_object_type_e ltype;


    if (physical)
        ltype = VINE_TYPE_PHYS_ACCEL;
    else
        ltype = VINE_TYPE_VIRT_ACCEL;

    vpipe = vine_pipe_get();

    acc_list =
      vine_object_list_lock(&(vpipe->objs), ltype);

    if (accels) { /* Want the accels */
        if (*accels)
            vine_accel_list_free_pre_locked(*accels);
        *accels = malloc( (acc_list->length + 1) * sizeof(vine_accel *) );
        acl     = (vine_accel_s **) *accels;
    }

    if (physical) {
        vine_accel_s *accel = 0;
        utils_list_for_each(*acc_list, itr){
            accel = (vine_accel_s *) itr->owner;
            if (!type || accel->type == type) {
                accel_count++;
                if (acl) {
                    vine_object_ref_inc(&(accel->obj));
                    *acl = accel;
                    acl++;
                }
            }
        }
    } else {
        vine_vaccel_s *accel = 0;
        utils_list_for_each(*acc_list, itr){
            accel = (vine_vaccel_s *) itr->owner;
            if (!type || accel->type == type) {
                accel_count++;
                if (acl) {
                    vine_object_ref_inc(&(accel->obj));
                    *acl = (vine_accel_s *) accel;
                    acl++;
                }
            }
        }
    }
    if (acl)
        *acl = 0;  // delimiter
    vine_object_list_unlock(&(vpipe->objs), ltype);

    return accel_count;
} /* vine_accel_list */

void vine_accel_list_free(vine_accel **accels)
{
    vine_object_s **itr = (vine_object_s **) accels;

    while (*itr) {
        vine_object_ref_dec(*itr);
        itr++;
    }
    free(accels);
}

void vine_accel_list_free_pre_locked(vine_accel **accels)
{
    vine_object_s **itr = (vine_object_s **) accels;

    while (*itr) {
        vine_object_ref_dec_pre_locked(*itr);
        itr++;
    }
    free(accels);
}

vine_accel_type_e vine_accel_type(vine_accel *accel)
{
    vine_accel_s *_accel;

    _accel = accel;

    return _accel->type;
}

vine_accel_state_e vine_accel_stat(vine_accel *accel, vine_accel_stats_s *stat)
{
    vine_accel_s *_accel;
    vine_accel_state_e ret;

    _accel = accel;

    switch (_accel->obj.type) {
        case VINE_TYPE_PHYS_ACCEL:
            ret = vine_accel_get_stat(_accel, stat);
            break;
        case VINE_TYPE_VIRT_ACCEL:
            ret = vine_vaccel_get_stat((vine_vaccel_s *) _accel, stat);
            break;
        default:
            ret = accel_failed; /* Not very 'correct' */
    }

    return ret;
}

int vine_accel_acquire_phys(vine_accel **accel)
{
    vine_pipe_s *vpipe;
    vine_accel_s *_accel;
    int return_value = 0;


    vpipe = vine_pipe_get();

    _accel = *accel;

    if (_accel->obj.type == VINE_TYPE_PHYS_ACCEL) {
        *accel       = vine_vaccel_init(vpipe, "FILL", _accel->type, _accel);
        return_value = 1;
    }

    return return_value;
}

vine_accel* vine_accel_acquire_type(vine_accel_type_e type)
{
    vine_pipe_s *vpipe;
    vine_accel_s *_accel = 0;

    vpipe = vine_pipe_get();

    _accel = (vine_accel_s *) vine_vaccel_init(vpipe, "FILL", type, 0);

    return (vine_accel *) _accel;
}

void vine_accel_release(vine_accel **accel)
{
    vine_vaccel_s *_accel;

    _accel = *accel;

    switch (_accel->obj.type) {
        case VINE_TYPE_PHYS_ACCEL:
        case VINE_TYPE_VIRT_ACCEL:
            vine_object_ref_dec(&(_accel->obj));
            *accel = 0;
            return;

        default:
            vine_assert(!"Non accelerator type passed in vine_accel_release");
    }
}

vine_proc* vine_proc_register(const char *func_name)
{
    vine_pipe_s *vpipe;
    vine_proc_s *proc = 0;


    vpipe = vine_pipe_get();
    proc  = vine_pipe_find_proc(vpipe, func_name);

    if (!proc) { /* Proc has not been declared */
        proc = vine_proc_init(&(vpipe->objs), func_name);
    }

    return proc;
}

vine_proc* vine_proc_get(const char *func_name)
{
    vine_pipe_s *vpipe = vine_pipe_get();
    vine_proc_s *proc  = vine_pipe_find_proc(vpipe, func_name);

    if (proc)
        vine_object_ref_inc(&(proc->obj));
    else
        fprintf(stderr, "Proc %s not found!\n", func_name);

    return proc;
}

int vine_proc_put(vine_proc *func)
{
    vine_proc_s *proc = func;
    /* Decrease user count */
    int return_value = vine_object_ref_dec(&(proc->obj));

    return return_value;
}

int check_semantics(size_t in_count, vine_data **input, size_t out_count,
  vine_data **output)
{
    size_t io_cnt;
    size_t dup_cnt;
    size_t all_io = out_count + in_count;
    vine_data_s *temp_data_1 = 0;
    vine_data_s *temp_data_2 = 0;

    for (io_cnt = 0; io_cnt < all_io; io_cnt++) {
        // Choose from input or output
        if (io_cnt < in_count)
            temp_data_1 = input[io_cnt];
        else
            temp_data_1 = output[io_cnt - in_count];
        // check Validity temp_data_1
        if (!temp_data_1) {
            fprintf(stderr, "NULL input #%lu\n", io_cnt);
            return 0;
        }
        if (temp_data_1->obj.type != VINE_TYPE_DATA) {
            fprintf(stderr, "Input #%lu not valid data\n", io_cnt);
            return 0;
        }
        // Check duplicates
        for (dup_cnt = 0; dup_cnt < all_io; dup_cnt++) {
            // Choose from input or output
            if (dup_cnt < in_count)
                temp_data_2 = input[dup_cnt];
            else
                temp_data_2 = output[dup_cnt - in_count];
            // check Validity temp_data_2
            if (!temp_data_2) {
                fprintf(stderr, "NULL input #%lu\n", dup_cnt);
                return 0;
            }
            if (temp_data_2->obj.type != VINE_TYPE_DATA) {
                fprintf(stderr, "Input #%lu not valid data\n", dup_cnt);
                return 0;
            }
        }
    }
    return 1;
} /* check_semantics */

vine_task* vine_task_issue(vine_accel *accel, vine_proc *proc, const void *host_init, size_t host_size,
  size_t in_count, vine_data **dev_in, size_t out_count,
  vine_data **dev_out)
{
    // printf("%s %s\n",__func__, ((vine_proc_s*)proc)->obj.name) ;

    vine_pipe_s *vpipe = vine_pipe_get();
    vine_task_msg_s *task;
    vine_data **dest;
    int cnt;

    vine_assert(check_semantics(in_count, dev_in, out_count, dev_out));

    task = vine_task_alloc(vpipe, accel, proc, host_size, in_count, out_count);

    vine_assert(task);

    if (host_size && host_init)
        memcpy(vine_task_host_data(task, host_size), host_init, host_size);

    task->stats.task_id = __sync_fetch_and_add(&(vine_state.task_uid), 1);

    dest = task->io;

    for (cnt = 0; cnt < in_count; cnt++, dest++) {
        if (!dev_in[cnt]) {
            fprintf(stderr, "Invalid input #%d\n", cnt);
            return 0;
        }
        *dest = dev_in[cnt];
        if (((vine_data_s *) *dest)->obj.type != VINE_TYPE_DATA) {
            fprintf(stderr, "Input #%d not valid data\n", cnt);
            return 0;
        }
        // printf("Input allocation \n");
        vine_data_input_init(*dest, accel);
        vine_data_annotate(*dest, "%s:in[%d]", ((vine_proc_s *) proc)->obj.name, cnt);
    }

    for (cnt = 0; cnt < out_count; cnt++, dest++) {
        *dest = dev_out[cnt];
        if (!*dest) {
            fprintf(stderr, "Invalid output #%d\n", cnt);
            return 0;
        }
        if (((vine_data_s *) *dest)->obj.type != VINE_TYPE_DATA) {
            fprintf(stderr, "Input #%d not valid data\n", cnt);
            return 0;
        }
        // printf("Output . \n" );
        vine_data_output_init(*dest, accel);

        // data annotate
        vine_data_annotate(*dest, "%s:out[%d]", ((vine_proc_s *) proc)->obj.name, cnt);
    }

    vine_task_submit(task);

    return task;
} /* vine_task_issue */

vine_task_state_e vine_task_issue_sync(vine_accel *accel, vine_proc *proc, void *host_init,
  size_t host_size, size_t in_count, vine_data **dev_in, size_t out_count,
  vine_data **dev_out)
{
    vine_task *task = vine_task_issue(accel, proc, host_init, host_size, in_count, dev_in, out_count, dev_out);
    vine_task_state_e status = vine_task_wait(task);

    vine_task_free(task);

    return status;
}

vine_task_state_e vine_task_stat(vine_task *task, vine_task_stats_s *stats)
{
    vine_task_msg_s *_task = task;
    vine_task_state_e ret  = 0;

    ret = _task->state;

    if (stats)
        memcpy(stats, &(_task->stats), sizeof(*stats));

    return ret;
}

vine_task_state_e vine_task_wait(vine_task *task)
{
    vine_task_msg_s *_task = task;

    vine_task_wait_done(_task);

    return _task->state;
}

void vine_task_free(vine_task *task)
{
    vine_task_msg_s *_task = task;

    vine_object_ref_dec(&(_task->obj));
}

vine_buffer_s VINE_BUFFER(size_t size)
{
    vine_pipe_s *vpipe = vine_pipe_get();

    vine_data_s *vd = vine_data_init(vpipe, size);

    return vd;
}
