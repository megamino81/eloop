#ifndef __E_LOOP_H__
#define __E_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#define g_new(type, num) (type *)malloc(sizeof(type) * (num))


typedef enum {
    ELOOP_READ = 1,
    ELOOP_WRITE = 2,
    ELOOP_EXCEPTION = 4,
} EloopCondition;

typedef int (*EloopCallback)(void *data);
typedef int (*EloopInputCallback)(int fd, EloopCondition cond, void *data);


int eloop_add_timeout(unsigned int time_slice, EloopCallback cb, void *data);
void eloop_remove_timeout(int tag);
int eloop_add_input(int fd, EloopCondition cond, EloopInputCallback cb, void *data);
void eloop_remove_input(int tag);
void eloop_main(void);
void eloop_quit(void);

#ifdef __cpplus__
}
#endif

#endif
