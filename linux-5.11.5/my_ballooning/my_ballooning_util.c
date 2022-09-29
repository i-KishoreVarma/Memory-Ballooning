#include <my_ballooning/my_ballooning_util.h>
#include <my_ballooning/my_ballooning.h>
#include <linux/rmap.h>
#include <linux/mm_types.h>
#include <linux/mm.h>

/**
 *  vm_swappiness is declared in this header file
 **/
#include <linux/swap.h>

typedef struct {
    int nr;
    struct vm_area_struct *ptr;
    int nr_max;
} find_nr_ref_t;


static bool find_nr_references_one(struct page *page, struct vm_area_struct *vma,
			unsigned long address, void *arg)
{
    find_nr_ref_t *item = (find_nr_ref_t*)arg;
    item->nr++;
    item->ptr = vma;
    item->nr_max--;
    if(!item->nr_max) return false;
    return true;
}

/*
    returns {
        nr: number of refernces for this page
        ptr: pointer to vm_area_struct of one of them.
    }

*/
find_nr_ref_t find_nr_references(struct page *page)
{
    int tmc = total_mapcount(page)+1;
    find_nr_ref_t item = {
        .nr = 0,
        .ptr = NULL,
        .nr_max = tmc
    };

    struct rmap_walk_control rwc = {
        .rmap_one = find_nr_references_one,
        .arg = (void *)&item,
        .anon_lock = page_lock_anon_vma_read,
    };

    rmap_walk(page,&rwc);

    return item;
}

//* PageAnon(page) only valid

struct mm_struct* find_mem_struct(struct page *page)
{
    int tmc = total_mapcount(page)+1;
    find_nr_ref_t ret = find_nr_references(page);
    struct vm_area_struct *vmas = ret.ptr;
    struct mm_struct *mm = vmas->vm_mm;
    if(ret.nr==tmc)
    {
        return mm;
    }
    return NULL;
}