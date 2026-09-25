/*===OmniBridgeOs/kernel/arch/x64/sandbox.h===*/
#ifndef OMNIBRIDGE_SANDBOX_H
#define OMNIBRIDGE_SANDBOX_H

#include <stdint.h>
#include "task.h"

#ifndef OB_ENETUNREACH
#define OB_ENETUNREACH (-101)
#endif

struct task_t *sandbox_create_process(struct task_t *parent,
                                      const char *target_path,
                                      const char *argv_text,
                                      uint8_t level);

int sandbox_kill(struct task_t *caller, uint64_t pid);

void sandbox_list_dump(void);

void sandbox_entry_trampoline(void *arg);

int sandbox_setup_fs(struct task_t *t);
void sandbox_cleanup_fs(struct task_t *t);

int sandbox_net_is_unreachable(void);

#endif /* OMNIBRIDGE_SANDBOX_H */