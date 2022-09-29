/**==============================================
 * ?                    ABOUT
 * @author      : Kishore Varma
 * @email       : kishorea@iisc.ac.in
 * @repo        : 
 * @createdOn   : 
 * @description : Header file for my ballooning
 *=============================================**/ 

#ifndef MY_BALLOONING
#include <linux/signal.h>
#include <linux/mm.h>

#define MY_BALLOONING

#define SIGBALLOONING (SIGRTMIN+2)

#define MY_BALLOONING_MSG "MY BALLOONING : "

/**
 *  
 *  Assuming Page Size of 4KB
 *  
 *  => 1 GB = 1 * ( 2 ^ 30 )
 *  => No of Page Frames =  2^30 / 2^12 = 2^18
 *  =>                   =  1 << 18
 *  
 **/

#define MY_BALLOONING_THRESHOLD_MIN_PAGES (1<<18)

extern void my_ballooning_handle_low_memory(void);
extern bool check_valid_process(struct page *page);
extern bool my_ballooning_disabled_annon_swapping(void);
extern bool my_ballooning_active(void);
unsigned long my_ballooing_shrink_all_memory(unsigned long nr_to_reclaim);
void deregister_if_registered(struct task_struct *tsk);
int mb_swapin_page(struct vm_fault *vmf);

#endif
