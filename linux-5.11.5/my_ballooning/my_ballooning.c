#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/signal_types.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <asm/siginfo.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include "./../mm/internal.h"
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/jiffies.h>
#include <linux/memcontrol.h>
#include <linux/mman.h>
#include <linux/mmzone.h>
#include <linux/mmu_notifier.h>
#include <linux/page_ref.h>
// #include <asm/tlbflush.h>
#include <my_ballooning/my_ballooning.h>
#include <my_ballooning/my_ballooning_util.h>
#include <my_ballooning/mb_drop_caches.h>
#include <linux/page-flags.h>
#include <linux/swapops.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>

/**
 *  vm_swappiness is declared in this header file
 **/
#include <linux/swap.h>

#include <linux/workqueue.h>

#define MY_BALLOONING_SWAP_DIRECTORY "/ballooning/"

typedef unsigned long Page_NR_t;
typedef unsigned long mb_swap_entry;
typedef unsigned long long ul;

#define SWAP_FILE_SIZE ((ul)1 << 30)
int mb_send_signal(int sigNr);
int nr_signals = 0;
int skip_signals = 0;
int prev_freed_pages[10];
int called_times;
int wake_time;
/**
 *  Assuming Only 1 process will be registered.
 * 	If more than 1 process will register we can maintain a linked list.
 * 
 * 	Ideal Use : Register and Deregister
 * 
 *  Note : Why pid instead of task*, Imagine a rogue process that registered exits
 * 	then memory associated with that task will be freed and we are accessing some random data.
 * 	so pid's are better, but problem is we need to get access to task* from pid.
 * 
 * 	Interesting Kernel maintains no of references to task*
 **/

typedef struct {
	ul *swapmap;
	ul size;
	ul first_free;
	ul last_free;
} mb_swap_info_t;

typedef struct {
	ul value;
} mb_swp_entry_t;


int mb_find_msb_set(ul val)
{
	int pos = 63;
	ul test_val = 1ul << pos;
	while (pos >= 0 && (val & test_val) != 0) {
		pos--;
		test_val >>= 1;
	}
	return 63-pos;
}

void mb_bit_set(ul *arr, ul bit_pos)
{
	ul pos = bit_pos / 64;
	ul off = 63ul-(bit_pos % 64);
	arr[pos] |= 1ul << off;
}

void mb_bit_unset(ul *arr, ul bit_pos)
{
	ul pos = bit_pos / 64;
	ul off = 63 - (bit_pos % 64);
	if(arr[pos]&( 1ul << off))
		arr[pos] ^= 1ul << off;
}

void freeSwapEntry(mb_swap_info_t *info,swp_entry_t entry)
{
	ul pos = swp_offset(entry);
	if(unlikely(pos>info->last_free)) return;
	mb_bit_unset(info->swapmap,pos);
	if(pos<info->first_free) info->first_free = pos;
}

swp_entry_t getSwapEntry(mb_swap_info_t *info)
{
	ul next_pos = (info->first_free / 64) * 64;
	int pos;
	ul found_slot = info->first_free;
	ul *arr= info->swapmap;
	if(info->first_free>info->last_free)
	{
		printk(MY_BALLOONING_MSG " Swap Full, So killing.\n");
		// Alternative Solution Bring Some Pages into Memory
		// Walk through Entire Page Table and bring back pages if enough memory is there
		// But at present Kill
		mb_send_signal(SIGKILL);
		return swp_entry(MAX_SWAPFILES,found_slot);
	}
	mb_bit_set(arr, found_slot);
	// if((found_slot%64)==0)
	// {
	// 	next_pos = found_slot+1;
	// }
	// else 
	for (; next_pos <= info->last_free; next_pos+=64) {
		pos = mb_find_msb_set(arr[next_pos/64]);
		if(pos!=64)
		{
			next_pos+=pos;
			break;
		}
	}
	if(next_pos>info->last_free){
		// bug => swap full => no next entry unless freed
	}
	info->first_free = next_pos;
	swp_entry_t entry = swp_entry(MAX_SWAPFILES,found_slot);
	return entry;
}

typedef struct {
	/* Registered Process */
	struct task_struct *process;

	/* if signal already sent stop repeatedly sending.  */
	int signal_sent,signal_received;

	/* Pages to swappout as suggested by programm */
	Page_NR_t *suggested_page_list;

	char *swapFile;

	struct file *swapFP;

	mb_swap_info_t swap_info;

	/* nr of suggested pages */
	int nr_pages;

	int nr_signals;

	spinlock_t infoLock;
	spinlock_t signalLock;

	int doing_swap_out;

} my_ballooning_info_t;

/**------------------------------------------------------------------------
 * *                                INFO
 *   
 *   Registered Process can be maintained as list for supporting multiple 
 * Processes.
 *   
 *------------------------------------------------------------------------**/
my_ballooning_info_t registered_process;


void my_work_handler(struct work_struct *work)
{
	if(!(registered_process.signal_received))
	{
		printk("Waited 10 seconds Didnt get any response back so killing\n");
		mb_send_signal(SIGKILL);
		return;
	}
	registered_process.signal_received = 0;
	registered_process.signal_sent = 0;
	printk("After 10 seconds\n");
}

DECLARE_DELAYED_WORK(my_work, my_work_handler);

/* whether ballooning system is active */
int my_ballooning_is_active = 1; 

void write_zero(mb_swap_info_t *info)
{
	ul * arr = info->swapmap;
	ul size = info->size;
	ul i=0;
	for(;i<size;i++) arr[i]=0;
	info->first_free = 0;
	info->last_free = size*sizeof(unsigned long)*8-1;
}

my_ballooning_info_t * mb_get_info(struct task_struct *task)
{
	int ret;
	if(registered_process.process!=NULL&&task==registered_process.process)
	{
		spin_lock(&registered_process.infoLock);
		return &registered_process;
	}
	return NULL;
}

void mb_put_info(my_ballooning_info_t *info)
{
	spin_unlock(&info->infoLock);
}

my_ballooning_info_t * mb_get_info_try(struct task_struct *task)
{
	int ret;
	if(registered_process.process!=NULL&&task==registered_process.process)
	{
		ret=spin_trylock(&registered_process.infoLock);
		if(ret) return &registered_process;
	}
	return NULL;
}

my_ballooning_info_t * mb_info_lock_try(my_ballooning_info_t *info)
{
	int ret=spin_trylock(&info->infoLock);
	if(ret) return info;
	return NULL;
}

void mb_info_lock(my_ballooning_info_t *info)
{
	spin_lock(&info->infoLock);
}

int mb_send_signal(int sigNr)
{
	struct kernel_siginfo kSignalInfo;

	memset(&kSignalInfo, 0, sizeof(struct kernel_siginfo));

	kSignalInfo.si_signo = sigNr;
	kSignalInfo.si_code = 0;
	kSignalInfo.si_int = 1234;
	int ret = send_sig_info(sigNr, &kSignalInfo,registered_process.process);

	return ret;
}

long mb_do_mkdirat1(int dfd, const char *pathname, umode_t mode)
{
	struct dentry *dentry;
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_DIRECTORY;

retry:
	dentry = kern_path_create(dfd, pathname, &path, lookup_flags);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	if (!IS_POSIXACL(path.dentry->d_inode))
		mode &= ~current_umask();
	error = security_path_mkdir(&path, dentry, mode);
	if (!error)
		error = vfs_mkdir(path.dentry->d_inode, dentry, mode);
	done_path_create(&path, dentry);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

void create_swap_file(my_ballooning_info_t *info)
{
	/**
	 *  
	 * Create Directory if not exists 
	 * 
	 **/
	struct file *fp = (struct file *)NULL;
	// fp = filp_open(MY_BALLOONING_SWAP_DIRECTORY, O_DIRECTORY | O_CREAT,
	// 	       0666);

	// if (IS_ERR(fp)) {
	// 	printk(MY_BALLOONING_MSG
	// 	       " Failed to Create Directory. Error Code : %d\n",
	// 	       PTR_ERR(fp));
	// 	// return;
	// } else
	// 	filp_close(fp, NULL);
		
	long err = mb_do_mkdirat1(AT_FDCWD, "/ballooning/", 0666);

	printk(MY_BALLOONING_MSG
		       " Failed to Create Directory. Error Code : %ld\n",
		       err);

	/**
	 *  Create Swap file with pid
	 **/

	char s[32];
	int ret = snprintf(s, 32, "/ballooning/swapfile_%d", registered_process.process->pid);
	if (ret > 32) {
		//file name truncated
	}
	info->swapFP = filp_open(s, O_CREAT | O_RDWR, 0666);
	if (IS_ERR(info->swapFP)) {
		printk(MY_BALLOONING_MSG
		       " Failed to Create Swap File. Error Code : %d\n",
		       PTR_ERR(fp));
		return;
	}
	// int error = vfs_fallocate(info->swapFP, 0, 0, SWAP_FILE_SIZE);
	// printk(MY_BALLOONING_MSG " Allocated swap file and error is : %d",
	//        error);
	

	if(info->swap_info.swapmap==NULL)
	{
		info->swap_info.swapmap = vmalloc(64*PAGE_SIZE);
		info->swap_info.size = 64*PAGE_SIZE/8;
		// at max 8GB Swap File
	}

	write_zero(&(info->swap_info));
}

/**------------------------------------------------------------------------
  * *                    Register My Ballooning
  *------------------------------------------------------------------------**/
SYSCALL_DEFINE0(register_my_ballooning)
{
	printk(MY_BALLOONING_MSG " Registered Application.\n");

	registered_process.signal_sent = 1;

	my_ballooning_is_active = 1;

	/**
	 *  
	 *  Swapping is disabled in get_scan_count [ vmscan.c file ] by
	 *  just asking it to check only file backed pages.  
	 *  
	 **/

	// my_ballooning_is_active = 0;

	// release past process by calling put_task_struct

	if (registered_process.process != NULL) {
		printk(MY_BALLOONING_MSG
		       " Something Went wrong task* should have been freed already\n");
		put_task_struct(registered_process.process);
	}

	registered_process.process = get_task_struct(current);

	create_swap_file(&registered_process);

	if (registered_process.suggested_page_list == NULL) {
		registered_process.suggested_page_list =
			vmalloc(sizeof(Page_NR_t) * 32 * PAGE_SIZE);
			// in single go we can swap 512MB
		if (!registered_process.suggested_page_list) {
			printk(MY_BALLOONING_MSG
			       " Failed to Allocate Memory\n");
		}
	}

	registered_process.signal_sent = 0;
	registered_process.signal_received = 0;
	registered_process.doing_swap_out = 0;
	skip_signals = 0;
	nr_signals = 0;
	spin_lock_init(&registered_process.infoLock);
	spin_lock_init(&registered_process.signalLock);
	int i;
	for(i=0;i<10;i++) prev_freed_pages[i] = 0;
	called_times = 0;
	wake_time = 1;
	return 0;
}

void deregister_if_registered(struct task_struct *tsk)
{

	my_ballooning_info_t *info = mb_get_info(tsk);

	if(info==NULL) return;

	printk(MY_BALLOONING_MSG
				" Cleaning Before Exiting \n");

	cancel_delayed_work_sync(&my_work);

	// struct inode *parent_inode = registered_process.swapFP->f_path.dentry->d_parent->d_inode;
	// inode_lock(parent_inode);
	// vfs_unlink(parent_inode, registered_process.swapFP->f_path.dentry, NULL);    
	// inode_unlock(parent_inode);
	
	filp_close(registered_process.swapFP, NULL);
	
	put_task_struct(registered_process.process);
	
	registered_process.process = NULL;
	// my_ballooning_is_active = 0;

	mb_put_info(info);
	
	printk(MY_BALLOONING_MSG
				" Cleaning Complete Proceeding Exiting \n");
}

bool my_ballooning_active(void)
{
	return my_ballooning_is_active;
}

bool my_ballooning_disabled_annon_swapping()
{
	if (my_ballooning_is_active)
		return 1;
	if (registered_process.process != NULL)
		return 1;
	return 0;
}

bool check_valid_process(struct page *page)
{
	struct mm_struct *mm = find_mem_struct(page);
	if (registered_process.process->mm != NULL &&
	    registered_process.process->mm == mm)
		return true;
	return false;
}

void my_ballooning_shrink_annon_memory(size_t nr_pages)
{
	// we need scan control for shrink
	// remove around 32*SWAP_CLUSTER_MAX = 32*32*4kB = 4mb
	// initially thought of doing shrink_list here
	// but it would block
	// while debugging found that kswapd => shrinkallmemory => try_free_pages => shrink..
	// but moving it here is needs to import a lot of headers and lot of work
	unsigned long nr_reclaimed = my_ballooing_shrink_all_memory(nr_pages);
	printk(MY_BALLOONING_MSG " Shrinked %lu .\n", nr_reclaimed);
}

enum ADVISE_RET_STATUS {
	SUCCESS,
	ECOPY,
	E_NOT_REGISTERED,
};

void my_wrapper(size_t nr_pages, Page_NR_t *page_list)
{
	int i, madvise_ret_val;

	struct mm_struct *mm = registered_process.process->mm;
	for (i = 0; i < nr_pages; ++i) {
		madvise_ret_val =
			do_madvise(mm, page_list[i], PAGE_SIZE, MADV_PAGEOUT);
		if (madvise_ret_val) {
			printk(MY_BALLOONING_MSG " Madvise Failed at %llu .\n",
			       page_list[i]);
		}
	}
}

static inline pmd_t *get_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep = NULL;

	pgd = pgd_offset(mm, addr);
	if (!pgd_present(*pgd))
		goto out;

	p4d = p4d_offset(pgd, addr);
	if (!p4d_present(*p4d))
		goto out;

	pud = pud_offset(p4d, addr);
	if (!pud_present(*pud))
		goto out;

	pmd = pmd_offset(pud, addr);
	if (!pmd_present(*pmd))
		return NULL;

out:
	return pmd;
}

static inline pte_t *get_ptep(struct mm_struct *mm, unsigned long addr)
{
	pmd_t *pmd = get_pmd(mm, addr);
	pte_t *ptep = NULL;

	if (pmd == NULL) {
		// BUG
		goto out;
	}

	// use below if necessary
	// struct vm_area_struct *vma = find_vma(mm, address);
	// need to hold lock
	// should use pte_offset_map_lock(vma->vm_mm, pmdp, address,
	// &vmf->ptl);

	ptep = pte_offset_map(pmd, addr);

	// no need to check here
	// if (!pte_present(*ptep))
	// 	return NULL;

out:
	return ptep;
}

void mb_free_page(struct page *page)
{
	page_remove_rmap(page,0);
	if (unlikely(page_mapcount(page) < 0))
	{
		printk("Very Bad");
		// mb_send_signal(SIGKILL);
		return;
	}
	put_page(page);
}
// int mb_free_page(struct page *page)
// {
// 	LIST_HEAD(free_pages);

// 	list_del(&page->lru);

// 	if (!trylock_page(page))
// 		goto keep;

// 	// if (page_mapped(page)) {
// 	// 	enum ttu_flags flags = TTU_BATCH_FLUSH;

// 	// 	if (!try_to_unmap(page, flags)) {
// 	// 		goto activate_locked;
// 	// 	}
// 	// }

// 	list_del(&page->lru);
// 	list_add(&page->lru, &free_pages);

// 	unlock_page(page);

// 	mem_cgroup_uncharge_list(&free_pages);
// 	try_to_unmap_flush();
// 	free_unref_page_list(&free_pages);
// 	return 0;
// keep:
// 	return 1;
// 	// do nothing failed to lock page
// activate_locked:
// 	// failed to do required operation
// 	unlock_page(page);
// 	return -1;
// }

bool mb_swapin_one(my_ballooning_info_t *info, struct page *page,
		   loff_t block_nr)
{
	struct file *fp = info->swapFP;

	// file offset -> Multiple of PAGE_SIZE
	loff_t off = block_nr * PAGE_SIZE;

	// address for that page in kernel space
	void *page_kaddr = page_to_virt(page);

	loff_t prevoff = info->swapFP->f_pos;

	int read_bytes = kernel_read(registered_process.swapFP, page_kaddr,
				     PAGE_SIZE, &off);
	
	info->swapFP->f_pos = prevoff;

	if (read_bytes < PAGE_SIZE) {
		printk(MY_BALLOONING_MSG " Read %d & offset is : %ld \n", read_bytes,off);
		return false;
	}
	return true;
}

pte_t mb_swapin_and_update(my_ballooning_info_t *info, unsigned long address)
{
	/*
	 *	Use Lock for PTEP by get_pte_lock
	 *  Use pos instead of i 
	 */

	// should remove this and replace with get swap entry
	static ul i = 0;

	struct mm_struct *mm = info->process->mm;

	struct vm_area_struct *vma = find_vma(mm, address);

	if (unlikely(!vma)) goto bad_vma_access;
	
	if (unlikely(vma->vm_start > address)) goto bad_vma_access;

	// Pointer to Page Table Entry
	pte_t *ptep = get_ptep(mm, address);

	if(ptep==NULL) goto bad_ptep_access;

	if(pte_present(*ptep)) goto bad_pte_access;

	pte_t entry;
	entry.pte = 0;

	swp_entry_t old_entry = pte_to_swp_entry(*ptep);

	if(swp_type(old_entry)!=MAX_SWAPFILES) goto bad_swp_type;

	struct page *page = alloc_zeroed_user_highpage_movable(vma, address);

	if(page==NULL) goto page_alloc_failed;

	//todo  Extract BlockNo from PTE

	if (!mb_swapin_one(info, page, swp_offset(old_entry))) {
		goto read_failed;
	}

	freeSwapEntry(&(info->swap_info),old_entry);

	// make PTE
	entry = mk_pte(page, vma->vm_page_prot);
	entry = pte_sw_mkyoung(entry);
	entry = maybe_mkwrite(pte_mkdirty(entry), vma);

	/**
	 *  
	 * Page Logistics starts
	 * 
	 **/

	ptep_clear_flush_notify(vma, address, ptep);
	page_add_new_anon_rmap(page, vma, address, 0);
	lru_cache_add_inactive_or_unevictable(page, vma);

	/**========================================================================
	 **                            Updating PTE
	 *========================================================================**/

	set_pte_at_notify(mm, address, ptep, entry);

	/**========================================================================
	 **                            Updating Done
	 *========================================================================**/

	update_mmu_cache(vma, address, ptep);
	update_mmu_tlb(vma, address, ptep);

	inc_mm_counter(vma->vm_mm, MM_ANONPAGES);
	dec_mm_counter(vma->vm_mm, MM_SWAPENTS);

	return entry;

bad_vma_access:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but VMA invalid so killing process\n", address);
	goto bad_send_signal;

bad_swp_type:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx Invalid SwapType => page not in our swap. Something Fishy\n", address);
	free_page(page);
	goto bad_send_signal;

read_failed:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but Reading From Swap Failed, So killing\n", address);
	goto bad_send_signal;

bad_pte_access:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but PTE is pointing to page so killing process\n", address);
	goto bad_send_signal;

page_alloc_failed:
	printk("Failed to Get new Page for (address) %lx & free Pages = %lu \n",address,nr_free_pages());
	goto bad_send_signal;

bad_ptep_access:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but PTEP invalid so killing process\n", address);

bad_send_signal:
	mb_send_signal(SIGKILL);
	return entry;
}


pte_t mb_swapin_and_update_test(my_ballooning_info_t *info, struct vm_fault *vmf)
{
	/*
	 *	Use Lock for PTEP by get_pte_lock
	 *  Use pos instead of i 
	 */

	// should remove this and replace with get swap entry
	static ul i = 0;

	struct mm_struct *mm = info->process->mm;

	struct vm_area_struct *vma = vmf->vma;

	// Pointer to Page Table Entry
	pte_t *ptep = vmf->pte;

	pte_t entry;
	entry.pte = 0;

	unsigned long address = vmf->address;

	if(ptep==NULL) goto bad_ptep_access;

	if(pte_present(*ptep)) goto bad_pte_access;

	swp_entry_t old_entry = pte_to_swp_entry(*ptep);

	if(swp_type(old_entry)!=MAX_SWAPFILES) goto bad_swp_type;

	mb_put_info(info);

	struct page *page = alloc_zeroed_user_highpage_movable(vma, address);
	
	mb_info_lock(info);

	if(page==NULL) goto page_alloc_failed;

	//todo  Extract BlockNo from PTE

	if (!mb_swapin_one(info, page, swp_offset(old_entry))) {
		goto read_failed;
	}

	freeSwapEntry(&(info->swap_info),old_entry);

	// make PTE
	entry = mk_pte(page, vma->vm_page_prot);
	entry = pte_sw_mkyoung(entry);
	entry = maybe_mkwrite(pte_mkdirty(entry), vma);

	/**
	 *  
	 * Page Logistics starts
	 * 
	 **/

	ptep_clear_flush_notify(vma, address, ptep);
	page_add_new_anon_rmap(page, vma, address, 0);
	lru_cache_add_inactive_or_unevictable(page, vma);

	/**========================================================================
	 **                            Updating PTE
	 *========================================================================**/

	set_pte_at_notify(mm, address, ptep, entry);

	/**========================================================================
	 **                            Updating Done
	 *========================================================================**/

	update_mmu_cache(vma, address, ptep);
	update_mmu_tlb(vma, address, ptep);

	inc_mm_counter(vma->vm_mm, MM_ANONPAGES);
	dec_mm_counter(vma->vm_mm, MM_SWAPENTS);

	return entry;

bad_vma_access:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but VMA invalid so killing process\n", address);
	goto bad_send_signal;

bad_swp_type:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx Invalid SwapType => page not in our swap. Something Fishy\n", address);
	goto bad_send_signal;

read_failed:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but Reading From Swap Failed, So killing\n", address);
	free_page(page_to_virt(page));
	goto bad_send_signal;

bad_pte_access:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but PTE is pointing to page so killing process\n", address);
	goto bad_send_signal;

page_alloc_failed:
	printk("Failed to Get new Page for (address) %lx & free Pages = %lu \n",address,nr_free_pages());
	goto bad_send_signal;

bad_ptep_access:
	printk(MY_BALLOONING_MSG " Tried To SwapIn Page %lx but PTEP invalid so killing process\n", address);

bad_send_signal:
	mb_send_signal(SIGKILL);
	return entry;
}

int mb_swapin_page(struct vm_fault *vmf)
{
	// acquires lock if valid
	my_ballooning_info_t *info = mb_get_info(current);

	if (info == NULL)
		return 0;

	// printk(MY_BALLOONING_MSG " Got Request to SWAP In %x\n", vmf->address);
	
	mb_swapin_and_update_test(info,vmf);

	// releases lock if acquired
	mb_put_info(info);

	return 1;
}

void mb_swapin_list(my_ballooning_info_t *info)
{
	size_t i;
	// acquire lock
	Page_NR_t *suggested_page_list = info->suggested_page_list;
	for (i = 0; i < info->nr_pages; i++) {
		mb_swapin_and_update(info, suggested_page_list[i]);
	}
}

// write given page to swap file at given offset
bool mb_swapout_one(my_ballooning_info_t *info, struct page *page,swp_entry_t entry)
{
	struct file *fp = info->swapFP;

	loff_t block_nr = swp_offset(entry);

	// file offset -> Multiple of PAGE_SIZE
	loff_t off = block_nr * PAGE_SIZE;

	// address for that page in kernel space
	void *page_kaddr = page_to_virt(page);

	loff_t prevoff = info->swapFP->f_pos;

	int written_bytes = kernel_write(info->swapFP, page_kaddr,
					 PAGE_SIZE, &off);
	
	

	if (written_bytes < PAGE_SIZE) {
		printk(MY_BALLOONING_MSG " Written %d Bytes\n", written_bytes);
		return false;
	}
	return true;
}

int mb_swapout_and_update(my_ballooning_info_t *info, Page_NR_t address)
{
	/*
	 *	Use Lock for PTEP by get_pte_lock
	 *  Use pos instead of i 
	 */

	struct mm_struct *mm = info->process->mm;

	struct vm_area_struct *vma = find_vma(mm, address);

	if (unlikely(!vma)) goto bad_vma_access;
	
	if (unlikely(vma->vm_start > address)) goto bad_vma_access;

	// Pointer to Page Table Entry
	pte_t *ptep = get_ptep(mm, address);

	if(ptep==NULL) goto bad_ptep_access;

	pte_t old_entry = *ptep;

	if(!pte_present(old_entry))
	{
		// something wrong trying to swapout swapped out page
		// printk(MY_BALLOONING_MSG " Tried To SwapOut Page %lx but its already Swapped\n", address);
		// goto bad_send_signal;
		return 4;
	}

	//  check if entry is swap-entry pte_to_swp_entry(old_entry);

	// use get swap entry
	//todo  TODO
	swp_entry_t swp_entry = getSwapEntry(&(info->swap_info));
	
	pte_t entry = swp_entry_to_pte(swp_entry);

	struct page *page = pte_page(old_entry);

	if(total_mapcount(page)>1||page_ref_count(page)>1)
	{
		// printk("Found Pages map count > 2 %lx\n",address);
		return 5;
	}
	if (!mb_swapout_one(info, page,swp_entry)) goto write_failed;

	/**
	 *  
	 * Removing Page Logic Starts
	 * 
	 **/

	flush_cache_page(vma, address, pte_pfn(old_entry));

	ptep_clear_flush_notify(vma, address, ptep);

	/**========================================================================
	 **                            Updating PTE
	 *========================================================================**/

	set_pte_at_notify(mm, address, ptep, entry);

	/**========================================================================
	 **                            Updating Done
	 *========================================================================**/

	mb_free_page(page);

	update_mmu_cache(vma, address, ptep);
	update_mmu_tlb(vma, address, ptep);

	// printk(MY_BALLOONING_MSG " Nr Free pages before %ld\n",
	//        nr_free_pages());

	/*================================ Freeing Page ==============================*/

	// put_page(page);

	dec_mm_counter(vma->vm_mm, MM_ANONPAGES);
	inc_mm_counter(vma->vm_mm, MM_SWAPENTS);


	// printk(MY_BALLOONING_MSG " Fred page %lx\n", address);

	return 1;

	// printk(MY_BALLOONING_MSG " Nr Free pages after %ld\n", nr_free_pages());
bad_vma_access:
	// printk(MY_BALLOONING_MSG " Tried To SwapOut Page %lx but VMA invalid so killing process\n", address);
	return 3;

write_failed:
	freeSwapEntry(&(info->swap_info),swp_entry);
	// printk(MY_BALLOONING_MSG " Tried To SwapOut Page %lx but Writing to Swap Failed, So killing\n", address);
	goto bad_send_signal;

bad_ptep_access:
	// printk(MY_BALLOONING_MSG " Tried To SwapOut Page %lx but PTEP invalid so ignoring request\n", address);
	return 2;
bad_send_signal:
	mb_send_signal(SIGKILL);
	return 0;
}

void mb_swapout_list(my_ballooning_info_t *info)
{
	size_t i=0;
	Page_NR_t *suggested_page_list = info->suggested_page_list;
	int nr_pages = info->nr_pages;
	int cc[8] = {0},tmp;
	
	// mb_put_info(info);

	printk(MY_BALLOONING_MSG " Free Pages Before : %ld\n", nr_free_pages());
	while(i<nr_pages)
	{
		// mb_info_lock(info);
		for (; i < info->nr_pages; i++) {
			tmp = mb_swapout_and_update(info, suggested_page_list[i]);
			if(tmp==0) break;
			cc[tmp]++;
			if((i%10000)==0)
			{
				i++;
				break;
			} 
		}
		// mb_put_info(info);
		if(tmp==0) break;
		// cond_resched();
	}
	mb_drop_caches();
	// drain_local_pages(NULL);
	// resched();
	// cond_resched();
	printk(MY_BALLOONING_MSG "Requested to swap out : %d\n",info->nr_pages);
	printk(MY_BALLOONING_MSG " Tried To SwapOut Pages but PTEP invalid so ignoring request: %d\n", cc[2]);
	printk(MY_BALLOONING_MSG " Tried To SwapOut Pages but VMA invalid so ignoring request: %d\n", cc[3]);
	printk(MY_BALLOONING_MSG "Found Pages map count > 2 : %d\n",cc[5]);
	printk(MY_BALLOONING_MSG "Swapout swapped Pages : %d\n",cc[4]);
	printk(MY_BALLOONING_MSG " Freed Pages %d\n", cc[1]);
	printk(MY_BALLOONING_MSG " Free Pages After : %ld\n", nr_free_pages());
	prev_freed_pages[called_times] = cc[1];
	called_times++;
	called_times%=10;
	// mb_info_lock(info);
}

int check_last_10_accesses(void)
{
	int swaps_done_last_5 = 0, swaps_done_recent_5 =0 , i;
	int last5 = called_times, last_access = (called_times-1);
	int j=5;
	static int retries = 0;
	if(last_access==-1) last_access = 0;
	while(j)
	{
		for(i=last5;i<10&&j;i++) swaps_done_last_5+=prev_freed_pages[i],j--;
		i%=10;
	}
	j=5;
	while(j)
	{
		for(;i<10&&j;i++) swaps_done_recent_5+=prev_freed_pages[i],j--;
		i%=10;
	}
	int diff;
	diff = swaps_done_recent_5-swaps_done_last_5;
	// if(diff==0)
	// {
	// 	wake_time=1;
	// 	return 0;
	// }
	// healthy;
	if(prev_freed_pages[last_access]>0)
	{
		wake_time--;
		wake_time = max(wake_time,1);
		retries = 0;
	}
	// else {
	// 	//should give some time more for app
	// 	wake_time+=2;
	// 	wake_time = min(wake_time,4); 
	// 	// return 0;
	// }
	if(swaps_done_recent_5==0){
		wake_time++;
		wake_time = min(wake_time,4); 
		retries++;
	}
	if(retries>20){
		mb_send_signal(SIGKILL);
	}
	// if(diff==0&&called_times>0)
	// {
	// 	wake_time++;
	// 	wake_time = min(wake_time,4); 
	// 	return 0;
	// }
	return 0;
}

SYSCALL_DEFINE2(my_ballooning_advise, const Page_NR_t __user *, user_page_list,
		size_t, nr_pages)
{
	enum ADVISE_RET_STATUS status = SUCCESS;

	my_ballooning_info_t *info = mb_get_info_try(current);

	if (info == NULL)
		return E_NOT_REGISTERED;

	if(info->doing_swap_out==1){
		printk(MY_BALLOONING_MSG " Received SwapoutList Request while prev is still executing so ignoring\n");
		mb_put_info(info);
		// nr_signals = -1;
		return -1;
	}
	info->doing_swap_out = 1;
	// if(nr_pages<=0) return -1;

	cancel_delayed_work_sync(&my_work);

	//give some time for app to breath
	info->signal_received = 1;


	printk(MY_BALLOONING_MSG " Called Advise.\n");


	// atmost 512MB is swapped in one go
	// => nr pages = 128k atmost
	info->nr_pages = min(nr_pages, PAGE_SIZE*32);

	// Returns nr of bytes it wasn't able to copy
	ul copy_fail =
		copy_from_user(info->suggested_page_list,
			       user_page_list, sizeof(Page_NR_t) * info->nr_pages);

	if (copy_fail) {
		printk(MY_BALLOONING_MSG " Failed to Copy user passed Data.\n");
		status = ECOPY;
		goto out;
	}

	mb_swapout_list(info);
	check_last_10_accesses();
	// schedule_delayed_work(&my_work, wake_time*HZ); 

	// mb_write_to_swap(nr_pages,registered_process.suggested_page_list);
	// mb_read_from_swap(nr_pages,registered_process.suggested_page_list);
	// changePage(current->mm,registered_process.suggested_page_list[1]);

	// my_wrapper(nr_pages,registered_process.suggested_page_list);

	// my_ballooning_shrink_annon_memory(nr_pages);
	// __

out:
	// nr_signals = 0;
	info->signal_sent = 0;
	info->doing_swap_out = 0;
	mb_put_info(info);
	return (int)status;
}

/**------------------------------------------------------------------------
 *					Handle When System Memory is Low
 *
 * Kernel Timers, Work Queues etc.,
 * https://www.oreilly.com/library/view/linux-device-drivers/0596005903/ch07.html
 *------------------------------------------------------------------------**/
void my_ballooning_handle_low_memory()
{

	// return;
	static unsigned long prevTimeStamp = 0;
	unsigned long curTimeStamp ;

	curTimeStamp = ktime_get_ns();

	// 500ms 1 signal
	if((curTimeStamp-prevTimeStamp)<wake_time*500000000ll) return;

	my_ballooning_info_t *info = mb_get_info_try(registered_process.process);


	if (info == NULL) 
		return;
	
	// if(registered_process.process==NULL) return;

	/**
	 *  Issue Signal to registered Process if it exists
	 **/

	if (registered_process.signal_sent)
	{
		// if(nr_free_pages()<10000)
		// {
		// 	if((skip_signal%10)==0)
		// 	{
		// 		skip_signal++;
		// 		use_skip_signal=1;
		// 	} 
		// }
		// else goto release_lock;
		goto release_lock;
	}

	// nr_signals++;

	// nr_signals%=5;

	// // out of 5 signals only 1 is passed
	// if(nr_signals!=1) goto release_lock;

	// can only send 1 signals in 1 sec 
	// if(curTimeStamp-prevTimeStamp < HZ/10) goto release_lock;


	// if((skip_signals%10)!=1) goto release_lock; 
	// for every valid 5000 signals send only 1
	// if((nr_signals%5000)!=1) goto release_lock;

	if (mb_send_signal(SIGBALLOONING) < 0) {
		// Failed to send
		printk(MY_BALLOONING_MSG
				" Failed to send signal to Application.\n");

	} else {
		printk(MY_BALLOONING_MSG
				" Signal Sent to Application.\n");
		registered_process.signal_sent = 1;
		schedule_delayed_work(&my_work, 10*HZ);
	}
	prevTimeStamp = ktime_get_ns();

	release_lock:
		mb_put_info(info);
	return;
}