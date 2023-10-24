#include "ftl.h"
#include <linux/kernel.h>

static void *ftl_thread(void *arg);
static int do_gc(struct ssd *ssd, bool force, int ch, int way);


// gc 필요한지 ch way 돌면서 free blk 갯수 확인 비교
static inline bool should_gc(struct ssd *ssd, int ch, int way)
{
    return (ssd->bm.free_queue_head[ch*ssd->sp.luns_per_ch+way].free_blk_cnt<= ssd->sp.gc_thres_blks);
}


// free blk에 병목이 생겼는지 확인
static inline bool should_gc_high(struct ssd *ssd, int ch, int lun)
{
    return (ssd->bm.free_queue_head[ch*ssd->sp.luns_per_ch+lun].free_blk_cnt <= ssd->sp.gc_thres_blks_high);
}

// lpn에 대한 ppa 값 반환
static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline struct r2w get_r2w_map(struct ssd *ssd, int region)
{
    return ssd->r2w_map[region];
}

static inline void set_r2w_map(struct ssd *ssd, int region, int ch, int lun)
{
    ssd->r2w_map[region].ch = ch;
    ssd->r2w_map[region].lun = lun;
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}


static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

// free, used queue의 꼬리에 node 매달아줌 더블 링크드 리스트로 관리
static void insert_queue_tail(struct blk *blk, struct ssd *ssd, int idx, int type) 
{
    struct blk_list_node *node = g_malloc0(sizeof(struct blk_list_node));
    struct blk_mgmt *bm = &ssd->bm;
    node->blk = blk;
    node->prev = NULL;
    node->next = NULL;
    
    if (type == 0) {
        if (bm->free_queue_head[idx].head->next == bm->free_queue_head[idx].tail)
        {
            bm->free_queue_head[idx].head->next = node;
            bm->free_queue_head[idx].tail->prev = node;
            node->prev = bm->free_queue_head[idx].head;
            node->next = bm->free_queue_head[idx].tail;
        }
        else
        {
            node->prev = bm->free_queue_head[idx].tail->prev;
            node->next = bm->free_queue_head[idx].tail;
            node->prev->next = node;
            bm->free_queue_head[idx].tail->prev = node;
        }
    }
    else if (type == 1) {
        if (bm->used_queue_head[idx].head->next == bm->used_queue_head[idx].tail)
        {
            bm->used_queue_head[idx].num_nodes++;
            bm->used_queue_head[idx].head->next = node;
            bm->used_queue_head[idx].tail->prev = node;
            node->prev = bm->used_queue_head[idx].head;
            node->next = bm->used_queue_head[idx].tail;
        }
        else
        {
            bm->used_queue_head[idx].num_nodes++;
            node->prev = bm->used_queue_head[idx].tail->prev;
            node->next = bm->used_queue_head[idx].tail;
            node->prev->next = node;
            bm->used_queue_head[idx].tail->prev = node;
        }
    }
}

// 큐에서 노드 제거 및 재연결
static void remove_queue(struct blk *blk, struct ssd *ssd, int idx, int type)
{
    struct blk_mgmt *bm = &ssd->bm;
    struct blk_list_node *temp;
    if (type == 0) {
        temp = bm->free_queue_head[idx].head->next;
    }
    else if (type == 1) {
        bm->used_queue_head[idx].num_nodes--;
        temp = bm->used_queue_head[idx].head->next;
    }

    if (temp == NULL) {
        fprintf(stderr, "Error: Block not found in the queue.\n");
    }
    while(temp->blk != NULL && temp->blk->id != blk->id)
    {
        temp = temp->next;
    }
    if (temp == NULL) {
        fprintf(stderr, "Error: Block not found \n");
    }
    temp->prev->next = temp->next;
    temp->next->prev = temp->prev;
    
}


// blk ssd가 init 될 때 ch, way마다 링크드 리스트 생성 및 연결
static void ssd_init_bm(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct blk_mgmt *bm = &ssd->bm;
    struct blk *blk;
    int cnt = 0;
    bm->tt_blks = spp->blks_per_pl * spp->luns_per_ch * spp->pls_per_lun * spp->nchs;
    ftl_assert(bm->tt_blks == spp->tt_blks);

    bm->free_queue_head = (linked_list*)g_malloc0(sizeof(linked_list)*spp->nchs*spp->luns_per_ch);

    bm->used_queue_head = (linked_list*)g_malloc0(sizeof(linked_list)*spp->nchs*spp->luns_per_ch);

    for(int i = 0; i < spp->nchs*spp->luns_per_ch; i++) {

        struct blk_list_node *dummy_head = g_malloc0(sizeof(struct blk_list_node));
        struct blk_list_node *dummy_tail = g_malloc0(sizeof(struct blk_list_node));

        dummy_head->next = dummy_tail;
        dummy_head->prev = NULL;
        dummy_head->blk = NULL;
        dummy_tail->prev = dummy_head;
        dummy_tail->next = NULL;
        dummy_tail->blk = NULL;
        bm->free_queue_head[i].head = dummy_head;
        bm->free_queue_head[i].tail = dummy_tail;
        bm->free_queue_head[i].num_nodes = 0;



        struct blk_list_node *used_dummy_head = g_malloc0(sizeof(struct blk_list_node));
        struct blk_list_node *used_dummy_tail = g_malloc0(sizeof(struct blk_list_node));
        used_dummy_head->next = used_dummy_tail;
        used_dummy_head->prev = NULL;
        used_dummy_head->blk = NULL;
        used_dummy_tail->prev = used_dummy_head;
        used_dummy_tail->next = NULL;
        used_dummy_tail->blk = NULL;
        bm->used_queue_head[i].head = used_dummy_head;
        bm->used_queue_head[i].tail = used_dummy_tail;
        bm->used_queue_head[i].num_nodes = 0;

    }
    bm->free_blk_cnt = 0;
	bm->used_blk_cnt = 0;
    // blk의 id를 순서대로 지정하여 blk 추적 용이하게 만들어줌
    for (int i = 0; i < spp->nchs; i++) {
        for (int j = 0; j < spp->luns_per_ch; j++) {
            for (int k = 0; k < spp->blks_per_pl; k++) {
                blk = &ssd->ch[i].lun[j].pl[0].blk[k];
                blk->id = cnt;
                cnt++;
                insert_queue_tail(blk, ssd, i*spp->luns_per_ch+j, 0);
                bm->free_queue_head[i*spp->luns_per_ch+j].free_blk_cnt++;
            }
        }
    }
    ftl_assert(bm->free_blk_cnt == bm->tt_blks);
}

// write_pointer init ch, way, 현재 blk, pg, free 여부등을 생성
static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct blk_mgmt *bm = &ssd->bm;
    ssd->wp = (struct write_pointer*)g_malloc0(sizeof(struct write_pointer)*spp->nchs*spp->luns_per_ch);
    struct write_pointer *wpp = ssd->wp;
    for (int i = 0; i < spp->nchs; i++) {
        for (int j = 0; j < spp->luns_per_ch; j++) {
            wpp[i*spp->luns_per_ch+j].ch = i;
            wpp[i*spp->luns_per_ch+j].lun = j;
            wpp[i*spp->luns_per_ch+j].curblk = bm->free_queue_head[i*spp->luns_per_ch+j].head->next->blk;
            wpp[i*spp->luns_per_ch+j].pg = 0;
            wpp[i*spp->luns_per_ch+j].blk = wpp[i*spp->luns_per_ch+j].curblk->id % spp->blks_per_ch % spp->blks_per_lun % spp->blks_per_pl;
            wpp[i*spp->luns_per_ch+j].pl = 0;
            wpp[i*spp->luns_per_ch+j].is_free = 1;
            remove_queue(wpp[i*spp->luns_per_ch+j].curblk, ssd, i*spp->luns_per_ch+j, 0);
            bm->free_queue_head[i*spp->luns_per_ch+j].free_blk_cnt--;
        }
    }
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

// 다음 free blk 탐색
static void get_next_free_blk(struct ssd *ssd, int ch, int lun)
{
    struct write_pointer *wpp = ssd->wp;
    struct blk_mgmt *bm = &ssd->bm;
    struct ssdparams *spp = &ssd->sp;
    wpp[ch*spp->luns_per_ch+lun].curblk = bm->free_queue_head[ch * spp->luns_per_ch + lun].head->next->blk;
    if (!wpp[ch*spp->luns_per_ch+lun].curblk) {
        ftl_err("No free blks left in [%s] !!!!\n", ssd->ssdname);
    }
    wpp[ch*spp->luns_per_ch+lun].blk = wpp[ch*spp->luns_per_ch+lun].curblk->id % spp->blks_per_ch % spp->blks_per_lun % spp->blks_per_pl;
    wpp[ch*spp->luns_per_ch+lun].pg = 0;
    remove_queue(wpp[ch*spp->luns_per_ch+lun].curblk, ssd, ch*spp->luns_per_ch + lun, 0);
    bm->free_queue_head[ch*spp->luns_per_ch+lun].free_blk_cnt--;
}

// write_pointer에 달려있는 블럭의 pg를 할당해주며 pg가 꽉차면 새로운 blk 할당
static void ssd_advance_write_pointer(struct ssd *ssd, int ch, int lun, int region)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = ssd->wp;
    struct blk_mgmt *bm = &ssd->bm;
    check_addr(wpp[ch*spp->luns_per_ch+lun].pg, spp->pgs_per_blk);
    int cw = spp->ch_way;
    // printf("ch : %d, lun : %d, blk : %d, pg : %d\n", wpp[ch*spp->luns_per_ch+lun].ch, wpp[ch*spp->luns_per_ch+lun].lun, wpp[ch*spp->luns_per_ch+lun].blk, wpp[ch*spp->luns_per_ch+lun].pg);
    wpp[ch*spp->luns_per_ch+lun].pg++;
    if (wpp[ch*spp->luns_per_ch+lun].pg == spp->pgs_per_blk) {
        
        insert_queue_tail(wpp[ch*spp->luns_per_ch+lun].curblk, ssd, ch*spp->luns_per_ch+lun, 1);
		bm->used_blk_cnt++;

        wpp[ch*spp->luns_per_ch+lun].curblk = NULL;
        while(!wpp[cw].is_free)
        {
            cw++;
            if (cw == spp->nchs * spp->luns_per_ch)
            {
                cw = 0;
            }
        }
        spp->ch_way = cw;
        set_r2w_map(ssd, region, cw/spp->luns_per_ch, cw%spp->luns_per_ch);
        wpp[cw].is_free = 0;
        wpp[ch*spp->luns_per_ch+lun].is_free = 1;

		get_next_free_blk(ssd, ch, lun);
        
		if (!wpp[ch*spp->luns_per_ch+lun].curblk) {

			abort();
		}
		ftl_assert(wpp[ch*spp->luns_per_ch+lun].pg == 0);

    }
}

// 새로운 페이지 받아오기
static struct ppa get_new_page(struct ssd *ssd, int ch, int lun)
{
    struct ppa ppa;
    struct write_pointer *wpp = ssd->wp;
    ppa.ppa = 0;
    ppa.g.ch = ch;
    ppa.g.lun = lun;
    ppa.g.pg = wpp[ch*ssd->sp.luns_per_ch+lun].pg;
    ppa.g.blk = wpp[ch*ssd->sp.luns_per_ch+lun].blk;
    ppa.g.pl = wpp[ch*ssd->sp.luns_per_ch+lun].pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

// static void check_params(struct ssdparams *spp)
// {
// }
// ssd init시 파라미터 설정(8GB)
static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 256;
    spp->blks_per_pl = 256; 
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 4;


    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;
    spp->data_xfer_lat = DATA_TRANSFER_LATENCY;
    spp->cmd_lat = CMD_LATENCY;

    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    spp->blks_per_line = spp->tt_luns; 
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; 

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_blks = (int)((1 - spp->gc_thres_pcent) * spp->blks_per_lun);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->gc_thres_blks_high = (int)((1 - spp->gc_thres_pcent_high) * spp->blks_per_lun);
    spp->enable_gc_delay = true;
    spp->num_region = spp->nchs * spp->luns_per_ch / 2;
    spp->region_size = spp->tt_pgs / spp->num_region;
    spp->ch_way = 0;
    check_params(spp);
}

// page, blk등 init 메모리 할당
static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_blk(struct blk *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct blk) * pl->nblks);

    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_blk(&pl->blk[i], spp);

    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

// 매핑 테이블 생성
static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}
// write pointer와 region 맵 생성
static void ssd_init_r2wmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    ssd->r2w_map = g_malloc0(sizeof(struct r2w) * spp->num_region);
    for (int i = 0; i < spp->num_region; i++) {
        ssd->r2w_map[i].ch = -1;
        ssd->r2w_map[i].lun = -1;
        // wpp[ssd->r2w_map[i].ch*spp->luns_per_ch+ssd->r2w_map[i].lun].is_free = 0;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

// ssd init
void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    ssd->num_ios = 0;
    ssd->valid_pgs = 0;
    ftl_assert(ssd);

    ssd_init_params(spp);


    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    ssd->cur_ch = 0;
    ssd->cur_lun = 0;

    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }


    ssd_init_maptbl(ssd);


    ssd_init_rmap(ssd);



    //ssd_init_lines(ssd);
	ssd_init_bm(ssd);


    ssd_init_write_pointer(ssd);
    ssd_init_r2wmap(ssd);
    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

// valid_ppa 체크
static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct blk *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct blk *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

// ssd에 IO 요청이 들어오면 확인
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
        // READ 요청에 대해 lat 측정
    case NAND_READ:
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                    lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat+ spp->cmd_lat;
            for (int i = 0; i < spp->nchs; i++)
            {   
                for (int j = 0; j < spp->luns_per_ch; j++)
                {
                    if (ppa->g.ch == i && ppa->g.lun == j)
                        continue;
                    struct ppa tmp_ppa;
                    tmp_ppa.g.ch = i;
                    tmp_ppa.g.lun = j;
                    struct nand_lun *tmp_lun = get_lun(ssd, &tmp_ppa);
                    tmp_lun->next_lun_avail_time += spp->cmd_lat;
                }
            }
        }
        else {
            lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:

        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat + spp->cmd_lat + spp->data_xfer_lat;
            for (int i = 0; i < spp->nchs; i++)
            {   
                for (int j = 0; j < spp->luns_per_ch; j++)
                {
                    if (ppa->g.ch == i && ppa->g.lun == j)
                        continue;
                    else if (ppa->g.ch == i)
                    {
                        struct ppa tmp_ppa;
                        tmp_ppa.g.ch = i;
                        tmp_ppa.g.lun = j;
                        struct nand_lun *tmp_lun = get_lun(ssd, &tmp_ppa);
                        tmp_lun->next_lun_avail_time += spp->cmd_lat + spp->data_xfer_lat;
                    }
                    else
                    {
                        struct ppa tmp_ppa;
                        tmp_ppa.g.ch = i;
                        tmp_ppa.g.lun = j;
                        struct nand_lun *tmp_lun = get_lun(ssd, &tmp_ppa);
                        tmp_lun->next_lun_avail_time += spp->cmd_lat;
                    }
                }
            }
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }

        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:

        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}


// page invalid 처리
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_page *pg = NULL;
	struct blk *blk = NULL;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;
}

// page valid 처리
static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_page *pg = NULL;
	struct blk *blk;
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;
}

// free blk 처리하기 위해 블럭 내부 페이지들 전부 프리로 변환 후 파라미터 초기화
static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
	struct blk_mgmt *bm = &ssd->bm;
    struct ssdparams *spp = &ssd->sp;
    struct blk *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
	insert_queue_tail(blk, ssd, ppa->g.ch*spp->luns_per_ch+ppa->g.lun, 0);
	bm->free_queue_head[ppa->g.ch*spp->luns_per_ch+ppa->g.lun].free_blk_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

// gc시 valid page copy
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    struct ssdparams *spp = &ssd->sp;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    ftl_assert(valid_lpn(ssd, lpn));
    int region;
    struct r2w wp_info;
    region = lpn / spp->region_size;
    wp_info = get_r2w_map(ssd, region);
    new_ppa = get_new_page(ssd, wp_info.ch, wp_info.lun);

    set_maptbl_ent(ssd, lpn, &new_ppa);

    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    ssd_advance_write_pointer(ssd, new_ppa.g.ch, new_ppa.g.lun, region);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

// victim_blk 선정
static struct blk *select_victim_blk(struct ssd *ssd, bool force, int ch, int way)
{
    struct blk_mgmt *bm = &ssd->bm;
    struct blk *victim_blk = NULL;
	struct ssdparams *spp = &ssd->sp;
	int max_ipc = 0;
    struct blk_list_node *cur_node = bm->used_queue_head[ch*spp->luns_per_ch+way].head;
    for (int k = 0; k < bm->used_queue_head[ch*spp->luns_per_ch+way].num_nodes; k++)
    {
        cur_node = cur_node->next;
        if (cur_node->blk->ipc == spp->pgs_per_blk)
        {
            remove_queue(cur_node->blk, ssd, ch*spp->luns_per_ch+way, 1);
            victim_blk = cur_node->blk;
            bm->used_blk_cnt--;
            return victim_blk;
        }
        if (cur_node->blk->ipc > max_ipc)
        {
            victim_blk = cur_node->blk;
            max_ipc = cur_node->blk->ipc;
        }
    }
	if(!victim_blk)
	{
		return NULL;
	}
    if (!force && victim_blk->ipc < ssd->sp.pgs_per_blk / 8) {
        return NULL;
    }
	remove_queue(victim_blk, ssd, ch*spp->luns_per_ch+way, 1);
	bm->used_blk_cnt--;
    return victim_blk;
}


// blk 초기화
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            gc_write_page(ssd, ppa);
            ssd->valid_pgs++;
            cnt++;
        }
    }
    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

// gc 실행 victim_blk 선정 후 해당 blk valid page copy하고 지워줌
static int do_gc(struct ssd *ssd, bool force, int ch, int way)
{
    struct blk *victim_blk = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
	int remain = 0;
    victim_blk = select_victim_blk(ssd, force, ch, way);
    if (!victim_blk) {
        return -1;
    }

	ppa.g.ch = ch; // 300 / 2048 -> 0
	remain = victim_blk->id % spp->blks_per_ch; // 300
	ppa.g.lun = way; // 300 / 256 => 1
	remain = remain % spp->blks_per_lun; // 44 / 256
	ppa.g.pl = 0;
	ppa.g.blk = remain % spp->blks_per_pl; //   / 256

	ppa.g.pg = 0;
    ftl_debug("GC-ing ipc=%d,free=%d\n", ppa.g.blk,
              victim_blk->ipc, ssd->bm.free_blk_cnt);
    lunp = get_lun(ssd, &ppa);
	clean_one_block(ssd, &ppa);
	mark_block_free(ssd, &ppa);

	if (spp->enable_gc_delay) {
		struct nand_cmd gce;
		gce.type = GC_IO;
		gce.cmd = NAND_ERASE;
		gce.stime = 0;
		ssd_advance_status(ssd, &ppa, &gce);
	}
    lunp->gc_endtime = lunp->next_lun_avail_time;

    return 0;
}


// read 요청에 대한 처리
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{

    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            // ssd->valid_pgs++;
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    return maxlat;
}


// write 요청에 대한 처리
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    int region;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    struct write_pointer *wpp = ssd->wp;
    struct r2w wp_info;
    int cw = spp->ch_way;
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        region = lpn / spp->region_size;
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        wp_info = get_r2w_map(ssd, region);
        if (wp_info.ch == -1)
        {
            while(!wpp[cw].is_free)
            {
                cw++;
                if (cw == spp->nchs * spp->luns_per_ch)
                {
                    cw = 0;
                }
            }
            spp->ch_way = cw;
            set_r2w_map(ssd, region, cw/spp->luns_per_ch, cw%spp->luns_per_ch);
            wpp[cw].is_free = 0;
            wp_info.ch = cw/spp->luns_per_ch;
            wp_info.lun = cw%spp->luns_per_ch;
        }
        struct ppa tmp_ppa;
        tmp_ppa.g.ch = wp_info.ch;
        tmp_ppa.g.lun = wp_info.lun;

        int r = 0;
        while (should_gc_high(ssd, tmp_ppa.g.ch, tmp_ppa.g.lun))
        {
            r = do_gc(ssd, true, tmp_ppa.g.ch, tmp_ppa.g.lun);
            if (r == -1)
                break;
        }
        ppa = get_new_page(ssd, tmp_ppa.g.ch, tmp_ppa.g.lun);

        set_maptbl_ent(ssd, lpn, &ppa);

        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        ssd_advance_write_pointer(ssd, ppa.g.ch, ppa.g.lun, region);
        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;
    struct ppa ppa;
    int cnt = 0;
    uint64_t temp_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            ssd->num_ios++;
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            for (int i = 0; i < ssd->sp.nchs; i++)
            {
                for (int j = 0; j < ssd->sp.luns_per_ch; j++)
                {
                    if (should_gc(ssd, i, j))
                        do_gc(ssd, false, i, j);
                }
            }
        }
        if (temp_time < qemu_clock_get_ns(QEMU_CLOCK_REALTIME))
        {
            temp_time += 5000000000;
            cnt = 0;
            for (int j = 0; j < ssd->sp.tt_pgs; j++)
            {
                ppa = get_maptbl_ent(ssd, j);
                if (mapped_ppa(&ppa)) {
                    cnt++;
                }
            }
            printf("num_ios = %d, valid = %d\n", ssd->num_ios, ssd->valid_pgs);
            // printf("sec =%d, spp = %d, ppb = %d, bpp = %d, ppl = %d, %d, %d", ssd->sp.secsz, ssd->sp.secs_per_pg, ssd->sp.pgs_per_blk, ssd->sp.blks_per_pl, ssd->sp.pls_per_lun, ssd->sp.luns_per_ch, ssd->sp.nchs);
        }
    }

    return NULL;
}
