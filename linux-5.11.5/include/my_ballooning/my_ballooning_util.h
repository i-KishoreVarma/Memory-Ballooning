#ifndef MY_BALLOONING_UTIL

#define MY_BALLOONING_UTIL

    #include <linux/kernel.h>
    #include <linux/sched.h>

    extern struct mm_struct* find_mem_struct(struct page *page);

#endif