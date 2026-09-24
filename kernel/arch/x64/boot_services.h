#ifndef OMNIBRIDGE_BOOT_SERVICES_H
#define OMNIBRIDGE_BOOT_SERVICES_H

#include "obinit.h"

/* 按 obinit 配置启动系统服务 task。
 * 返回 0 成功；负错误码表示第一个失败的服务（后续不再启动）。 */
int boot_services_start(struct obinit_config *cfg);

/* 使用内置默认 4 个服务（Init/SecMgr/ServiceHost/Auditd）启动。
 * 作为 obinit.toml 不存在时的回退路径。 */
int boot_services_default(void);

#endif /* OMNIBRIDGE_BOOT_SERVICES_H */