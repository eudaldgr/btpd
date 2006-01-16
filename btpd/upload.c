#include <string.h>

#include "btpd.h"

#define CHOKE_INTERVAL (& (struct timeval) { 10, 0 })

static struct event m_choke_timer;
static unsigned m_npeers;
static struct peer_tq m_peerq = BTPDQ_HEAD_INITIALIZER(m_peerq);
static int m_max_downloaders = 4;

struct peer_sort {
    struct peer *p;
    unsigned i;
};

static int
rate_cmp(const void *arg1, const void *arg2)
{
    struct peer *p1 = ((struct peer_sort *)arg1)->p;
    struct peer *p2 = ((struct peer_sort *)arg2)->p;
    unsigned long rate1 = cm_full(p1->n->tp) ? p1->rate_up : p1->rate_dwn;
    unsigned long rate2 = cm_full(p2->n->tp) ? p2->rate_up : p2->rate_dwn;
    if (rate1 < rate2)
        return -1;
    else if (rate1 == rate2)
        return 0;
    else
        return 1;
}

static void
choke_do(void)
{
    if (m_max_downloaders == -1) {
        struct peer *p;
        BTPDQ_FOREACH(p, &m_peerq, ul_entry)
            if (p->flags & PF_I_CHOKE)
                peer_unchoke(p);
    } else if (m_max_downloaders == 0) {
        struct peer *p;
        BTPDQ_FOREACH(p, &m_peerq, ul_entry)
            if ((p->flags & PF_I_CHOKE) == 0)
                peer_choke(p);
    } else if (m_npeers > 0) {
        struct peer_sort worthy[m_npeers];
        int nworthy = 0;
        int i = 0;
        int found = 0;
        struct peer *p;
        int unchoked[m_npeers];

        BTPDQ_FOREACH(p, &m_peerq, ul_entry) {
            if (!peer_full(p) &&
                ((cm_full(p->n->tp) && p->rate_up > 0)
                    || (!cm_full(p->n->tp) && p->rate_dwn > 0))) {
                worthy[nworthy].p = p;
                worthy[nworthy].i = i;
                nworthy++;
            }
            i++;
        }
        qsort(worthy, nworthy, sizeof(worthy[0]), rate_cmp);

        bzero(unchoked, sizeof(unchoked));
        for (i = nworthy - 1; i >= 0 && found < m_max_downloaders - 1; i--) {
            if ((worthy[i].p->flags & PF_P_WANT) != 0)
                found++;
            if ((worthy[i].p->flags & PF_I_CHOKE) != 0)
                peer_unchoke(worthy[i].p);
            unchoked[worthy[i].i] = 1;
        }

        i = 0;
        BTPDQ_FOREACH(p, &m_peerq, ul_entry) {
            if (!unchoked[i]) {
                if (found < m_max_downloaders && !peer_full(p)) {
                    if (p->flags & PF_P_WANT)
                        found++;
                    if (p->flags & PF_I_CHOKE)
                        peer_unchoke(p);
                } else {
                    if ((p->flags & PF_I_CHOKE) == 0)
                        peer_choke(p);
                }
            }
            i++;
        }
    }
}

static void
shuffle_optimists(void)
{
    for (int i = 0; i < m_npeers; i++) {
        struct peer *p = BTPDQ_FIRST(&m_peerq);
        if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == (PF_P_WANT|PF_I_CHOKE)) {
            break;
        } else {
            BTPDQ_REMOVE(&m_peerq, p, ul_entry);
            BTPDQ_INSERT_TAIL(&m_peerq, p, ul_entry);
        }
    }
}

static void
choke_cb(int sd, short type, void *arg)
{
    evtimer_add(&m_choke_timer, CHOKE_INTERVAL);
    static int cb_count = 0;
    cb_count++;
    if (cb_count % 3 == 0)
        shuffle_optimists();
    choke_do();
}

void
ul_on_new_peer(struct peer *p)
{
    long where = rand_between(-2, m_npeers);
    if (where < 1)
        BTPDQ_INSERT_HEAD(&m_peerq, p, ul_entry);
    else {
        struct peer *it = BTPDQ_FIRST(&m_peerq);
        where--;
        while (where > 0) {
            it = BTPDQ_NEXT(it, ul_entry);
            where--;
        }
        BTPDQ_INSERT_AFTER(&m_peerq, it, p, ul_entry);
    }
    m_npeers++;
    choke_do();
}

void
ul_on_lost_peer(struct peer *p)
{
    assert(m_npeers > 0);
    BTPDQ_REMOVE(&m_peerq, p, ul_entry);
    m_npeers--;
    if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT)
        choke_do();
}

void
ul_on_lost_torrent(struct net *n)
{
    struct peer *p;
    BTPDQ_FOREACH(p, &n->peers, p_entry) {
        BTPDQ_REMOVE(&m_peerq, p, ul_entry);
        m_npeers--;
    }
    choke_do();
}

void
ul_on_interest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
        choke_do();
}

void
ul_on_uninterest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
        choke_do();
}

void
ul_init(void)
{
    evtimer_set(&m_choke_timer, choke_cb, NULL);
    evtimer_add(&m_choke_timer, CHOKE_INTERVAL);
}