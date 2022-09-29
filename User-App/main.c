#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include "testcases.h"

void *buff;
unsigned long nr_signals = 0;

#define PAGE_SIZE		(4096)

#define SIGBALLOON SIGRTMIN

#define MY_BALLOONING_SYS_NO 548
#define MY_BALLOONING_ADVISE 549
/*
 * 			placeholder-3
 * implement your page replacement policy here
 */


/*
 * 			placeholder-2
 * implement your signal handler here
 */

#define BATCH_SIZE (1<<15)

unsigned long int advise_pages[BATCH_SIZE];

void my_ballooning_handler(int sig)
{
	if(buff==NULL)
	{
		 // syscall(MY_BALLOONING_ADVISE,advise_pages,0); 
		return;
	}
	printf("SignaL Handler Starting:\n");
	nr_signals+=1;
	static int i=0;
	int nr_pages = TOTAL_MEMORY_SIZE/PAGE_SIZE;
	int nr_batches = nr_pages/BATCH_SIZE;
	if(nr_pages%BATCH_SIZE) nr_batches++;
	//for(int i=0;i<nr_batches;i++)
	//{
		advise_pages[0] =(unsigned long) (buff)+ i*(PAGE_SIZE*BATCH_SIZE);
		int nr_max = BATCH_SIZE;
		unsigned long astart = i*(PAGE_SIZE*BATCH_SIZE);
		unsigned long aend = astart+nr_max*PAGE_SIZE;
		if(aend>TOTAL_MEMORY_SIZE) nr_max-= (aend-TOTAL_MEMORY_SIZE)/PAGE_SIZE;
		for(int k=1;k<nr_max;k++)
				advise_pages[k] = advise_pages[k-1]+PAGE_SIZE;
        printf("Calling System Call\n");
		syscall(MY_BALLOONING_ADVISE,advise_pages,nr_max);
	//}
	i++;
	i%=nr_batches;
	printf("Done Advising\n");
}

int main(int argc, char *argv[])
{
	int *ptr, nr_pages;

    	ptr = mmap(NULL, TOTAL_MEMORY_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

	if (ptr == MAP_FAILED) {
		printf("mmap failed\n");
       		exit(1);
	}
	buff = ptr;

	memset(buff, 0, TOTAL_MEMORY_SIZE);

	signal(SIGBALLOON, my_ballooning_handler); 

	/*
	 * 		placeholder-1
	 * register me with the kernel ballooning subsystem
	 */

	struct timeval start, end;
    double time_taken_for_syscall;

	gettimeofday(&start, NULL);

	syscall(MY_BALLOONING_SYS_NO);

	gettimeofday(&end, NULL);

	time_taken_for_syscall = 1e6 * ( end.tv_sec - start.tv_sec ) + (double)( end.tv_usec - start.tv_usec ) ;

	printf("Time Taken for execution of SYS call %lfus \n",time_taken_for_syscall);

// 	memset(buff, 0, TOTAL_MEMORY_SIZE);

	/* test-case */
	test_case_main(buff, TOTAL_MEMORY_SIZE);

	munmap(ptr, TOTAL_MEMORY_SIZE);
	printf("I received SIGBALLOON %lu times\n", nr_signals);
}
