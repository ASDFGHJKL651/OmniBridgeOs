/*===OmniBridgeOs/kernel/arch/x64/mem_domain_test.h===*/
#ifndef OMNIBRIDGE_MEM_DOMAIN_TEST_H
#define OMNIBRIDGE_MEM_DOMAIN_TEST_H

/*
 * 第 15 步：mem_domain 自检入口。
 * 由 main.c 在 oshell_test() 之后、sched_init() 之前调用。
 */
void mem_domain_test(void);

#endif /* OMNIBRIDGE_MEM_DOMAIN_TEST_H */
/*===OmniBridgeOs/kernel/arch/x64/mem_domain_test.h 结束===*/