/**
 * @file
 * This is the IPv4 packet segmentation and reassembly implementation.
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Jani Monoses <jani@iv.ro>
 *         Simon Goldschmidt
 * original reassembly code by Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"

#if LWIP_IPV4

#include "lwip/ip4_frag.h"
#include "lwip/def.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include "lwip/icmp.h"

#include <string.h>

#if IP_REASSEMBLY
/**
 * The IP reassembly code currently has the following limitations:
 * - IP header options are not supported
 * - fragments must not overlap (e.g. due to different routes),
 *   currently, overlapping or duplicate fragments are thrown away
 *   if IP_REASS_CHECK_OVERLAP=1 (the default)!
 *
 * @todo: work with IP header options
 */

/** Setting this to 0, you can turn off checking the fragments for overlapping
 * regions. The code gets a little smaller. Only use this if you know that
 * overlapping won't occur on your network! */
#ifndef IP_REASS_CHECK_OVERLAP
#define IP_REASS_CHECK_OVERLAP 1
#endif /* IP_REASS_CHECK_OVERLAP */

/** Set to 0 to prevent freeing the oldest datagram when the reassembly buffer is
 * full (IP_REASS_MAX_PBUFS pbufs are enqueued). The code gets a little smaller.
 * Datagrams will be freed by timeout only. Especially useful when MEMP_NUM_REASSDATA
 * is set to 1, so one datagram can be reassembled at a time, only. */
#ifndef IP_REASS_FREE_OLDEST
#define IP_REASS_FREE_OLDEST 1
#endif /* IP_REASS_FREE_OLDEST */

#define IP_REASS_FLAG_LASTFRAG 0x01

#define IP_REASS_VALIDATE_TELEGRAM_FINISHED  1
#define IP_REASS_VALIDATE_PBUF_QUEUED        0
#define IP_REASS_VALIDATE_PBUF_DROPPED       -1

/** This is a helper struct which holds the starting
 * offset and the ending offset of this fragment to
 * easily chain the fragments.
 * It has the same packing requirements as the IP header, since it replaces
 * the IP header in memory in incoming fragments (after copying it) to keep
 * track of the various fragments. (-> If the IP header doesn't need packing,
 * this struct doesn't need packing, too.)
 */
#ifdef PACK_STRUCT_USE_INCLUDES
#  include "arch/bpstruct.h"
#endif
PACK_STRUCT_BEGIN
struct ip_reass_helper {
  PACK_STRUCT_FIELD(struct pbuf *next_pbuf);
  PACK_STRUCT_FIELD(u16_t start);
  PACK_STRUCT_FIELD(u16_t end);
} PACK_STRUCT_STRUCT;
PACK_STRUCT_END
#ifdef PACK_STRUCT_USE_INCLUDES
#  include "arch/epstruct.h"
#endif

#define IP_ADDRESSES_AND_ID_MATCH(iphdrA, iphdrB)  \
  (ip4_addr_cmp(&(iphdrA)->src, &(iphdrB)->src) && \
   ip4_addr_cmp(&(iphdrA)->dest, &(iphdrB)->dest) && \
   IPH_ID(iphdrA) == IPH_ID(iphdrB)) ? 1 : 0

/*全局变量*/
static struct ip_reassdata *reassdatagrams;
static u16_t ip_reass_pbufcount;

/*函数原型*/ 
static void ip_reass_dequeue_datagram(struct ip_reassdata *ipr, struct ip_reassdata *prev);
static int ip_reass_free_complete_datagram(struct ip_reassdata *ipr, struct ip_reassdata *prev);

/**
 * 重组计时器基本功能
 * 对于NO_SYS == 0和1（！）。
 *
 * 应每1000毫秒调用一次（由IP_TMR_INTERVAL定义）。
 */ 
void
ip_reass_tmr(void)
{
  struct ip_reassdata *r, *prev = NULL;

  r = reassdatagrams;
  while (r != NULL) {
    /*减少计时器数值。一旦达到0，则清理不完整的片段组件*/
    if (r->timer > 0) {
      r->timer--;
      LWIP_DEBUGF(IP_REASS_DEBUG, ("ip_reass_tmr: timer dec %"U16_F"\n", (u16_t)r->timer));
      prev = r;
      r = r->next;
    } else {
      /* 重组发生超时 */ 
      struct ip_reassdata *tmp;
      LWIP_DEBUGF(IP_REASS_DEBUG, ("ip_reass_tmr: timer timed out\n"));
      tmp = r;
      /* 在释放前获取下一个指针 */
      r = r->next;
      /* 释放辅助结构和所有入队的pbuf */
      ip_reass_free_complete_datagram(tmp, prev);
    }
  }
}

/**
 * 释放数据报（struct ip_reassdata）及其所有pbuf。
 * 更新入队pbufs的总数（ip_reass_pbufcount），
 * SNMP计数并发送ICMP超时数据包。
 *
 * @param ipr数据报免费
 * @param在链表中显示以前的数据报
 * @return释放的pbuf数量
 */ 
static int
ip_reass_free_complete_datagram(struct ip_reassdata *ipr, struct ip_reassdata *prev)
{
  u16_t pbufs_freed = 0;
  u16_t clen;
  struct pbuf *p;
  struct ip_reass_helper *iprh;

  LWIP_ASSERT("prev != ipr", prev != ipr);
  if (prev != NULL) {
    LWIP_ASSERT("prev->next == ipr", prev->next == ipr);
  }

  MIB2_STATS_INC(mib2.ipreasmfails);
#if LWIP_ICMP
  iprh = (struct ip_reass_helper *)ipr->p->payload;
  if (iprh->start == 0) {
    /* 收到第一个片段, send ICMP time exceeded. */
    /* 首先，获取从 ipr-> p中排队第一个pbuf。*/
    p = ipr->p;
    ipr->p = iprh->next_pbuf;
    SMEMCPY(p->payload, &ipr->iphdr, IP_HLEN);
    icmp_time_exceeded(p, ICMP_TE_FRAG);
    clen = pbuf_clen(p);
    LWIP_ASSERT("pbufs_freed + clen <= 0xffff", pbufs_freed + clen <= 0xffff);
    pbufs_freed = (u16_t)(pbufs_freed + clen);
    pbuf_free(p);
  }
#endif /* LWIP_ICMP */

  /* 首先，释放所有收到的pbuf。个别pbuf需要单独发布，因为它们尚未被链接 */
  p = ipr->p;
  while (p != NULL) {
    struct pbuf *pcur;
    iprh = (struct ip_reass_helper *)p->payload;
    pcur = p;
    /* 在释放前获取下一个指针 */
    p = iprh->next_pbuf;
    clen = pbuf_clen(pcur);
    LWIP_ASSERT("pbufs_freed + clen <= 0xffff", pbufs_freed + clen <= 0xffff);
    pbufs_freed = (u16_t)(pbufs_freed + clen);
    pbuf_free(pcur);
  }
  /* 然后，解除列表中的struct ip_reassdata并释放它 */
  ip_reass_dequeue_datagram(ipr, prev);
  LWIP_ASSERT("ip_reass_pbufcount >= pbufs_freed", ip_reass_pbufcount >= pbufs_freed);
  ip_reass_pbufcount = (u16_t)(ip_reass_pbufcount - pbufs_freed);

  return pbufs_freed;
}

#if IP_REASS_FREE_OLDEST
/**
 * 释放最旧的数据报，为排队新片段腾出空间。
 * 'fraghdr'所属的数据报未被释放！
 *
 * @param fraghdr当前片段的IP头
 * @param pbufs_needed排队所需的pbuf数
 * （如果空间不足，用于释放其他数据报）
 * @return 释放的pbuf数量
 */ 
static int
ip_reass_remove_oldest_datagram(struct ip_hdr *fraghdr, int pbufs_needed)
{
  /* @todo Can't we simply remove the last datagram in the
   *       linked list behind reassdatagrams?
   */
  struct ip_reassdata *r, *oldest, *prev, *oldest_prev;
  int pbufs_freed = 0, pbufs_freed_current;
  int other_datagrams;

  /* 释放数据报，直到pbuf被允许排队'pbufs_needed'pbufs，但不释放'fraghdr'所属的数据报！ */
  do {
    oldest = NULL;
    prev = NULL;
    oldest_prev = NULL;
    other_datagrams = 0;
    r = reassdatagrams;
    while (r != NULL) {
      if (!IP_ADDRESSES_AND_ID_MATCH(&r->iphdr, fraghdr)) {
        /* 其他数据报 */
        other_datagrams++;
        if (oldest == NULL) {
          oldest = r;
          oldest_prev = prev;
        } else if (r->timer <= oldest->timer) {
          /* older than the previous oldest */
          oldest = r;
          oldest_prev = prev;
        }
      }
      if (r->next != NULL) {
        prev = r;
      }
      r = r->next;
    }
    if (oldest != NULL) {
      pbufs_freed_current = ip_reass_free_complete_datagram(oldest, oldest_prev);
      pbufs_freed += pbufs_freed_current;
    }
  } while ((pbufs_freed < pbufs_needed) && (other_datagrams > 1));
  return pbufs_freed;
}
#endif /* IP_REASS_FREE_OLDEST */

/**
 * Enqueues a new fragment into the fragment queue
 * @param fraghdr points to the new fragments IP hdr
 * @param clen number of pbufs needed to enqueue (used for freeing other datagrams if not enough space)
 * @return A pointer to the queue location into which the fragment was enqueued
 */
/**
 * 将新片段排入片段队列
 * @param fraghdr指向新的片段IP hdr
 * @param清除入队所需的pbuf数量（用于释放其他数据报，如果没有足够的空间）
 * @return指向片段入队的队列位置的指针
 */ 
static struct ip_reassdata *
ip_reass_enqueue_new_datagram(struct ip_hdr *fraghdr, int clen)
{
  struct ip_reassdata *ipr;
#if ! IP_REASS_FREE_OLDEST
  LWIP_UNUSED_ARG(clen);
#endif

  /* No matching previous fragment found, allocate a new reassdata struct */
  /* 找不到匹配的先前片段，分配一个新的reassdata结构 */
  ipr = (struct ip_reassdata *)memp_malloc(MEMP_REASSDATA);
  if (ipr == NULL) {
#if IP_REASS_FREE_OLDEST
    if (ip_reass_remove_oldest_datagram(fraghdr, clen) >= clen) {
      ipr = (struct ip_reassdata *)memp_malloc(MEMP_REASSDATA);
    }
    if (ipr == NULL)
#endif /* IP_REASS_FREE_OLDEST */
    {
      IPFRAG_STATS_INC(ip_frag.memerr);
      LWIP_DEBUGF(IP_REASS_DEBUG, ("Failed to alloc reassdata struct\n"));
      return NULL;
    }
  }
  memset(ipr, 0, sizeof(struct ip_reassdata));
  ipr->timer = IP_REASS_MAXAGE;

  /* enqueue the new structure to the front of the list */
  /* 将新结构排入列表前面 */
  ipr->next = reassdatagrams;
  reassdatagrams = ipr;
  /* copy the ip header for later tests and input */
  /* 复制ip头以便以后的测试和输入 */ 
  /* @todo: no ip options supported? */
  SMEMCPY(&(ipr->iphdr), fraghdr, IP_HLEN);
  return ipr;
}

/**
 * Dequeues a datagram from the datagram queue. Doesn't deallocate the pbufs.
 * @param ipr points to the queue entry to dequeue
 */
/**
 * 从数据报队列中出列数据报。不释放pbuf。
 * @param ipr指向队列条目出列
 */ 
static void
ip_reass_dequeue_datagram(struct ip_reassdata *ipr, struct ip_reassdata *prev)
{
  /* dequeue the reass struct  */
  if (reassdatagrams == ipr) {
    /* it was the first in the list */
    reassdatagrams = ipr->next;
  } else {
    /* it wasn't the first, so it must have a valid 'prev' */
    /* 它不是第一个，所以它必须有一个有效的'prev' */
    LWIP_ASSERT("sanity check linked list", prev != NULL);
    prev->next = ipr->next;
  }

  /* now we can free the ip_reassdata struct */
  /* 现在我们可以释放ip_reassdata */ 
  memp_free(MEMP_REASSDATA, ipr);
}

/**
 * Chain a new pbuf into the pbuf list that composes the datagram.  The pbuf listwill grow over time as  new pbufs are rx.
 * Also checks that the datagram passes basic continuity checks (if the lastfragment was received at least once)
 * @param ipr points to the reassembly state
 * @param new_p points to the pbuf for the current fragment
 * @param is_last is 1 if this pbuf has MF==0 (ipr->flags not updated yet)
 * @return see IP_REASS_VALIDATE_* defines
 */
/**
 * 将新的pbuf链接到组成数据报的pbuf列表中。随着新的pbuf是rx，pbuf列表会随着时间的推移而增长。
 * 还检查数据报是否通过基本连续性检查（如果最后一个片段至少收到一次）
 * @param ipr指向重组状态
 * @param new_p指向当前片段的pbuf
 * 如果此pbuf具有MF == 0（@pr->标志尚未更新），则@param is_last为1
 * @return请参阅IP_REASS_VALIDATE_ *定义
 */
static int
ip_reass_chain_frag_into_datagram_and_validate(struct ip_reassdata *ipr, struct pbuf *new_p, int is_last)
{
  struct ip_reass_helper *iprh, *iprh_tmp, *iprh_prev = NULL;
  struct pbuf *q;
  u16_t offset, len;
  u8_t hlen;
  struct ip_hdr *fraghdr;
  int valid = 1;

  /* Extract length and fragment offset from current fragment */
  fraghdr = (struct ip_hdr *)new_p->payload;
  len = lwip_ntohs(IPH_LEN(fraghdr));
  hlen = IPH_HL_BYTES(fraghdr);
  if (hlen > len) {
    /* invalid datagram */
    return IP_REASS_VALIDATE_PBUF_DROPPED;
  }
  len = (u16_t)(len - hlen);
  offset = IPH_OFFSET_BYTES(fraghdr);

  /* overwrite the fragment's ip header from the pbuf with our helper struct,
   * and setup the embedded helper structure. */
  /* 使用我们的helper struct从pbuf覆盖片段的ip头，
   * 并设置嵌入式辅助结构。 */ 
  /* make sure the struct ip_reass_helper fits into the IP header */
  /* 确保struct ip_reass_helper适合IP头 */
  LWIP_ASSERT("sizeof(struct ip_reass_helper) <= IP_HLEN",
              sizeof(struct ip_reass_helper) <= IP_HLEN);
  iprh = (struct ip_reass_helper *)new_p->payload;
  iprh->next_pbuf = NULL;
  iprh->start = offset;
  iprh->end = (u16_t)(offset + len);
  if (iprh->end < offset) {
    /* u16_t overflow, cannot handle this */
    return IP_REASS_VALIDATE_PBUF_DROPPED;
  }

  /* Iterate through until we either get to the end of the list (append),
   * or we find one with a larger offset (insert). */
  /*迭代直到我们要么到达列表的末尾（追加），
   *或者我们找到一个偏移量较大的插入（插入）。 */
  for (q = ipr->p; q != NULL;) {
    iprh_tmp = (struct ip_reass_helper *)q->payload;
    if (iprh->start < iprh_tmp->start) {
        /*新的pbuf应该在此之前插入*/
      /* the new pbuf should be inserted before this */
      iprh->next_pbuf = q;
      if (iprh_prev != NULL) {
        /* 不是偏移量最小的片段 */ 
        /* not the fragment with the lowest offset */
#if IP_REASS_CHECK_OVERLAP
        if ((iprh->start < iprh_prev->end) || (iprh->end > iprh_tmp->start)) {
          /* 片段与之前或之后重叠，扔掉 */
          /* fragment overlaps with previous or following, throw away */
          return IP_REASS_VALIDATE_PBUF_DROPPED;
        }
#endif /* IP_REASS_CHECK_OVERLAP */
        iprh_prev->next_pbuf = new_p;
        if (iprh_prev->end != iprh->start) {
         /* 当前之间缺少一个片段和之前的片段 */ 
          /* There is a fragment missing between the current
           * and the previous fragment */
          valid = 0;
        }
      } else {
#if IP_REASS_CHECK_OVERLAP
        if (iprh->end > iprh_tmp->start) {
          /* 片段与以下重叠，扔掉 */ 
          /* fragment overlaps with following, throw away */
          return IP_REASS_VALIDATE_PBUF_DROPPED;
        }
#endif /* IP_REASS_CHECK_OVERLAP */
        /* fragment with the lowest offset */
        ipr->p = new_p;
      }
      break;
    } else if (iprh->start == iprh_tmp->start) {
        /* 两次收到相同的数据报：无需保留数据报 */
      /* received the same datagram twice: no need to keep the datagram */
      return IP_REASS_VALIDATE_PBUF_DROPPED;
#if IP_REASS_CHECK_OVERLAP
    } else if (iprh->start < iprh_tmp->end) {
        /* 重叠：无需保留新数据报 */
      /* overlap: no need to keep the new datagram */
      return IP_REASS_VALIDATE_PBUF_DROPPED;
#endif /* IP_REASS_CHECK_OVERLAP */
    } else {
      /* 检查到目前为止收到的碎片是否没有holes。 */
      /* Check if the fragments received so far have no holes. */
      if (iprh_prev != NULL) {
        if (iprh_prev->end != iprh_tmp->start) {
            /* 当前之间缺少一个片段和上一个片段 */ 
          /* There is a fragment missing between the current
           * and the previous fragment */
          valid = 0;
        }
      }
    }
    q = iprh_tmp->next_pbuf;
    iprh_prev = iprh_tmp;
  }

  /* If q is NULL, then we made it to the end of the list. Determine what to do now */
  if (q == NULL) {
    if (iprh_prev != NULL) {
      /* this is (for now), the fragment with the highest offset:
       * chain it to the last fragment */
#if IP_REASS_CHECK_OVERLAP
      LWIP_ASSERT("check fragments don't overlap", iprh_prev->end <= iprh->start);
#endif /* IP_REASS_CHECK_OVERLAP */
      iprh_prev->next_pbuf = new_p;
      if (iprh_prev->end != iprh->start) {
        valid = 0;
      }
    } else {
#if IP_REASS_CHECK_OVERLAP
      LWIP_ASSERT("no previous fragment, this must be the first fragment!",
                  ipr->p == NULL);
#endif /* IP_REASS_CHECK_OVERLAP */
      /* this is the first fragment we ever received for this ip datagram */
      ipr->p = new_p;
    }
  }

  /* At this point, the validation part begins: */
  /*至此，验证部分开始：*/ 
  /* If we already received the last fragment */
  /*如果我们已经收到最后一个片段*/
  if (is_last || ((ipr->flags & IP_REASS_FLAG_LASTFRAG) != 0)) {
    /* and had no holes so far */
    if (valid) {
      /* then check if the rest of the fragments is here */
      /* Check if the queue starts with the first datagram */
      if ((ipr->p == NULL) || (((struct ip_reass_helper *)ipr->p->payload)->start != 0)) {
        valid = 0;
      } else {
        /* and check that there are no holes after this datagram */
        iprh_prev = iprh;
        q = iprh->next_pbuf;
        while (q != NULL) {
          iprh = (struct ip_reass_helper *)q->payload;
          if (iprh_prev->end != iprh->start) {
            valid = 0;
            break;
          }
          iprh_prev = iprh;
          q = iprh->next_pbuf;
        }
        /* if still valid, all fragments are received
         * (because to the MF==0 already arrived */
        if (valid) {
          LWIP_ASSERT("sanity check", ipr->p != NULL);
          LWIP_ASSERT("sanity check",
                      ((struct ip_reass_helper *)ipr->p->payload) != iprh);
          LWIP_ASSERT("validate_datagram:next_pbuf!=NULL",
                      iprh->next_pbuf == NULL);
        }
      }
    }
    /* If valid is 0 here, there are some fragments missing in the middle
     * (since MF == 0 has already arrived). Such datagrams simply time out if
     * no more fragments are received... */
    return valid ? IP_REASS_VALIDATE_TELEGRAM_FINISHED : IP_REASS_VALIDATE_PBUF_QUEUED;
  }
  /* If we come here, not all fragments were received, yet! */
  return IP_REASS_VALIDATE_PBUF_QUEUED; /* not yet valid! */
}

/**
 * 将传入的IP片段重新组合为IP数据报。
 *
 *@param p指向片段的pbuf链
 *@return 如果重组不完整则返回NULL
 */ 
struct pbuf *
ip4_reass(struct pbuf *p)
{
  struct pbuf *r;
  struct ip_hdr *fraghdr;
  struct ip_reassdata *ipr;
  struct ip_reass_helper *iprh;
  u16_t offset, len, clen;
  u8_t hlen;
  int valid;
  int is_last;

  IPFRAG_STATS_INC(ip_frag.recv);
  MIB2_STATS_INC(mib2.ipreasmreqds);

  fraghdr = (struct ip_hdr *)p->payload;

  if (IPH_HL_BYTES(fraghdr) != IP_HLEN) {
    LWIP_DEBUGF(IP_REASS_DEBUG, ("ip4_reass: IP options currently not supported!\n"));
    IPFRAG_STATS_INC(ip_frag.err);
    goto nullreturn;
  }

  offset = IPH_OFFSET_BYTES(fraghdr);
  len = lwip_ntohs(IPH_LEN(fraghdr));
  hlen = IPH_HL_BYTES(fraghdr);
  if (hlen > len) {
    /* 无效的数据报 */
    goto nullreturn;
  }
  len = (u16_t)(len - hlen);

  /* 检查是否允许我们加入更多数据报。 */
  clen = pbuf_clen(p);
  if ((ip_reass_pbufcount + clen) > IP_REASS_MAX_PBUFS) {
#if IP_REASS_FREE_OLDEST
    if (!ip_reass_remove_oldest_datagram(fraghdr, clen) ||
        ((ip_reass_pbufcount + clen) > IP_REASS_MAX_PBUFS))
#endif /* IP_REASS_FREE_OLDEST */
    {
      /* 无法释放数据报，但仍然有太多的pbuf排队 */
      LWIP_DEBUGF(IP_REASS_DEBUG, ("ip4_reass: Overflow condition: pbufct=%d, clen=%d, MAX=%d\n",
                                   ip_reass_pbufcount, clen, IP_REASS_MAX_PBUFS));
      IPFRAG_STATS_INC(ip_frag.memerr);
      /* @todo: send ICMP time exceeded here? */
      /* drop this pbuf */
      goto nullreturn;
    }
  }

  /* Look for the datagram the fragment belongs to in the current datagram queue,
   * remembering the previous in the queue for later dequeueing. */
  for (ipr = reassdatagrams; ipr != NULL; ipr = ipr->next) {
    /* Check if the incoming fragment matches the one currently present
       in the reassembly buffer. If so, we proceed with copying the
       fragment into the buffer. */
    if (IP_ADDRESSES_AND_ID_MATCH(&ipr->iphdr, fraghdr)) {
      LWIP_DEBUGF(IP_REASS_DEBUG, ("ip4_reass: matching previous fragment ID=%"X16_F"\n",
                                   lwip_ntohs(IPH_ID(fraghdr))));
      IPFRAG_STATS_INC(ip_frag.cachehit);
      break;
    }
  }

  if (ipr == NULL) {
    /* Enqueue a new datagram into the datagram queue */
    ipr = ip_reass_enqueue_new_datagram(fraghdr, clen);
    /* Bail if unable to enqueue */
    if (ipr == NULL) {
      goto nullreturn;
    }
  } else {
    if (((lwip_ntohs(IPH_OFFSET(fraghdr)) & IP_OFFMASK) == 0) &&
        ((lwip_ntohs(IPH_OFFSET(&ipr->iphdr)) & IP_OFFMASK) != 0)) {
      /* ipr->iphdr is not the header from the first fragment, but fraghdr is
       * -> copy fraghdr into ipr->iphdr since we want to have the header
       * of the first fragment (for ICMP time exceeded and later, for copying
       * all options, if supported)*/
      SMEMCPY(&ipr->iphdr, fraghdr, IP_HLEN);
    }
  }

  /* At this point, we have either created a new entry or pointing
   * to an existing one */

  /* check for 'no more fragments', and update queue entry*/
  is_last = (IPH_OFFSET(fraghdr) & PP_NTOHS(IP_MF)) == 0;
  if (is_last) {
    u16_t datagram_len = (u16_t)(offset + len);
    if ((datagram_len < offset) || (datagram_len > (0xFFFF - IP_HLEN))) {
      /* u16_t overflow, cannot handle this */
      goto nullreturn_ipr;
    }
  }
  /* find the right place to insert this pbuf */
  /* @todo: trim pbufs if fragments are overlapping */
  valid = ip_reass_chain_frag_into_datagram_and_validate(ipr, p, is_last);
  if (valid == IP_REASS_VALIDATE_PBUF_DROPPED) {
    goto nullreturn_ipr;
  }
  /* if we come here, the pbuf has been enqueued */

  /* Track the current number of pbufs current 'in-flight', in order to limit
     the number of fragments that may be enqueued at any one time
     (overflow checked by testing against IP_REASS_MAX_PBUFS) */
  ip_reass_pbufcount = (u16_t)(ip_reass_pbufcount + clen);
  if (is_last) {
    u16_t datagram_len = (u16_t)(offset + len);
    ipr->datagram_len = datagram_len;
    ipr->flags |= IP_REASS_FLAG_LASTFRAG;
    LWIP_DEBUGF(IP_REASS_DEBUG,
                ("ip4_reass: last fragment seen, total len %"S16_F"\n",
                 ipr->datagram_len));
  }

  if (valid == IP_REASS_VALIDATE_TELEGRAM_FINISHED) {
    struct ip_reassdata *ipr_prev;
    /* the totally last fragment (flag more fragments = 0) was received at least
     * once AND all fragments are received */
    u16_t datagram_len = (u16_t)(ipr->datagram_len + IP_HLEN);

    /* save the second pbuf before copying the header over the pointer */
    r = ((struct ip_reass_helper *)ipr->p->payload)->next_pbuf;

    /* copy the original ip header back to the first pbuf */
    fraghdr = (struct ip_hdr *)(ipr->p->payload);
    SMEMCPY(fraghdr, &ipr->iphdr, IP_HLEN);
    IPH_LEN_SET(fraghdr, lwip_htons(datagram_len));
    IPH_OFFSET_SET(fraghdr, 0);
    IPH_CHKSUM_SET(fraghdr, 0);
    /* @todo: do we need to set/calculate the correct checksum? */
#if CHECKSUM_GEN_IP
    IF__NETIF_CHECKSUM_ENABLED(ip_current_input_netif(), NETIF_CHECKSUM_GEN_IP) {
      IPH_CHKSUM_SET(fraghdr, inet_chksum(fraghdr, IP_HLEN));
    }
#endif /* CHECKSUM_GEN_IP */

    p = ipr->p;

    /* chain together the pbufs contained within the reass_data list. */
    while (r != NULL) {
      iprh = (struct ip_reass_helper *)r->payload;

      /* hide the ip header for every succeeding fragment */
      pbuf_remove_header(r, IP_HLEN);
      pbuf_cat(p, r);
      r = iprh->next_pbuf;
    }

    /* find the previous entry in the linked list */
    if (ipr == reassdatagrams) {
      ipr_prev = NULL;
    } else {
      for (ipr_prev = reassdatagrams; ipr_prev != NULL; ipr_prev = ipr_prev->next) {
        if (ipr_prev->next == ipr) {
          break;
        }
      }
    }

    /* release the sources allocate for the fragment queue entry */
    ip_reass_dequeue_datagram(ipr, ipr_prev);

    /* and adjust the number of pbufs currently queued for reassembly. */
    clen = pbuf_clen(p);
    LWIP_ASSERT("ip_reass_pbufcount >= clen", ip_reass_pbufcount >= clen);
    ip_reass_pbufcount = (u16_t)(ip_reass_pbufcount - clen);

    MIB2_STATS_INC(mib2.ipreasmoks);

    /* Return the pbuf chain */
    return p;
  }
  /* the datagram is not (yet?) reassembled completely */
  LWIP_DEBUGF(IP_REASS_DEBUG, ("ip_reass_pbufcount: %d out\n", ip_reass_pbufcount));
  return NULL;

nullreturn_ipr:
  LWIP_ASSERT("ipr != NULL", ipr != NULL);
  if (ipr->p == NULL) {
    /* dropped pbuf after creating a new datagram entry: remove the entry, too */
    LWIP_ASSERT("not firstalthough just enqueued", ipr == reassdatagrams);
    ip_reass_dequeue_datagram(ipr, NULL);
  }

nullreturn:
  LWIP_DEBUGF(IP_REASS_DEBUG, ("ip4_reass: nullreturn\n"));
  IPFRAG_STATS_INC(ip_frag.drop);
  pbuf_free(p);
  return NULL;
}
#endif /* IP_REASSEMBLY */

#if IP_FRAG
#if !LWIP_NETIF_TX_SINGLE_PBUF
/** Allocate a new struct pbuf_custom_ref */
static struct pbuf_custom_ref *
ip_frag_alloc_pbuf_custom_ref(void)
{
  return (struct pbuf_custom_ref *)memp_malloc(MEMP_FRAG_PBUF);
}

/** Free a struct pbuf_custom_ref */
static void
ip_frag_free_pbuf_custom_ref(struct pbuf_custom_ref *p)
{
  LWIP_ASSERT("p != NULL", p != NULL);
  memp_free(MEMP_FRAG_PBUF, p);
}

/** Free-callback function to free a 'struct pbuf_custom_ref', called by
 * pbuf_free. */
static void
ipfrag_free_pbuf_custom(struct pbuf *p)
{
  struct pbuf_custom_ref *pcr = (struct pbuf_custom_ref *)p;
  LWIP_ASSERT("pcr != NULL", pcr != NULL);
  LWIP_ASSERT("pcr == p", (void *)pcr == (void *)p);
  if (pcr->original != NULL) {
    pbuf_free(pcr->original);
  }
  ip_frag_free_pbuf_custom_ref(pcr);
}
#endif /* !LWIP_NETIF_TX_SINGLE_PBUF */

/**
 * Fragment an IP datagram if too large for the netif.
 *
 * Chop the datagram in MTU sized chunks and send them in order
 * by pointing PBUF_REFs into p.
 *
 * @param p ip packet to send
 * @param netif the netif on which to send
 * @param dest destination ip address to which to send
 *
 * @return ERR_OK if sent successfully, err_t otherwise
 */
err_t
ip4_frag(struct pbuf *p, struct netif *netif, const ip4_addr_t *dest)
{
  struct pbuf *rambuf;
#if !LWIP_NETIF_TX_SINGLE_PBUF
  struct pbuf *newpbuf;
  u16_t newpbuflen = 0;
  u16_t left_to_copy;
#endif
  struct ip_hdr *original_iphdr;
  struct ip_hdr *iphdr;
  const u16_t nfb = (u16_t)((netif->mtu - IP_HLEN) / 8);
  u16_t left, fragsize;
  u16_t ofo;
  int last;
  u16_t poff = IP_HLEN;
  u16_t tmp;
  int mf_set;

  original_iphdr = (struct ip_hdr *)p->payload;
  iphdr = original_iphdr;
  if (IPH_HL_BYTES(iphdr) != IP_HLEN) {
    /* ip4_frag() does not support IP options */
    return ERR_VAL;
  }
  LWIP_ERROR("ip4_frag(): pbuf too short", p->len >= IP_HLEN, return ERR_VAL);

  /* Save original offset */
  tmp = lwip_ntohs(IPH_OFFSET(iphdr));
  ofo = tmp & IP_OFFMASK;
  /* already fragmented? if so, the last fragment we create must have MF, too */
  mf_set = tmp & IP_MF;

  left = (u16_t)(p->tot_len - IP_HLEN);

  while (left) {
    /* Fill this fragment */
    fragsize = LWIP_MIN(left, (u16_t)(nfb * 8));

#if LWIP_NETIF_TX_SINGLE_PBUF
    rambuf = pbuf_alloc(PBUF_IP, fragsize, PBUF_RAM);
    if (rambuf == NULL) {
      goto memerr;
    }
    LWIP_ASSERT("this needs a pbuf in one piece!",
                (rambuf->len == rambuf->tot_len) && (rambuf->next == NULL));
    poff += pbuf_copy_partial(p, rambuf->payload, fragsize, poff);
    /* make room for the IP header */
    if (pbuf_add_header(rambuf, IP_HLEN)) {
      pbuf_free(rambuf);
      goto memerr;
    }
    /* fill in the IP header */
    SMEMCPY(rambuf->payload, original_iphdr, IP_HLEN);
    iphdr = (struct ip_hdr *)rambuf->payload;
#else /* LWIP_NETIF_TX_SINGLE_PBUF */
    /* When not using a static buffer, create a chain of pbufs.
     * The first will be a PBUF_RAM holding the link and IP header.
     * The rest will be PBUF_REFs mirroring the pbuf chain to be fragged,
     * but limited to the size of an mtu.
     */
    rambuf = pbuf_alloc(PBUF_LINK, IP_HLEN, PBUF_RAM);
    if (rambuf == NULL) {
      goto memerr;
    }
    LWIP_ASSERT("this needs a pbuf in one piece!",
                (rambuf->len >= (IP_HLEN)));
    SMEMCPY(rambuf->payload, original_iphdr, IP_HLEN);
    iphdr = (struct ip_hdr *)rambuf->payload;

    left_to_copy = fragsize;
    while (left_to_copy) {
      struct pbuf_custom_ref *pcr;
      u16_t plen = (u16_t)(p->len - poff);
      LWIP_ASSERT("p->len >= poff", p->len >= poff);
      newpbuflen = LWIP_MIN(left_to_copy, plen);
      /* Is this pbuf already empty? */
      if (!newpbuflen) {
        poff = 0;
        p = p->next;
        continue;
      }
      pcr = ip_frag_alloc_pbuf_custom_ref();
      if (pcr == NULL) {
        pbuf_free(rambuf);
        goto memerr;
      }
      /* Mirror this pbuf, although we might not need all of it. */
      newpbuf = pbuf_alloced_custom(PBUF_RAW, newpbuflen, PBUF_REF, &pcr->pc,
                                    (u8_t *)p->payload + poff, newpbuflen);
      if (newpbuf == NULL) {
        ip_frag_free_pbuf_custom_ref(pcr);
        pbuf_free(rambuf);
        goto memerr;
      }
      pbuf_ref(p);
      pcr->original = p;
      pcr->pc.custom_free_function = ipfrag_free_pbuf_custom;

      /* Add it to end of rambuf's chain, but using pbuf_cat, not pbuf_chain
       * so that it is removed when pbuf_dechain is later called on rambuf.
       */
      pbuf_cat(rambuf, newpbuf);
      left_to_copy = (u16_t)(left_to_copy - newpbuflen);
      if (left_to_copy) {
        poff = 0;
        p = p->next;
      }
    }
    poff = (u16_t)(poff + newpbuflen);
#endif /* LWIP_NETIF_TX_SINGLE_PBUF */

    /* Correct header */
    last = (left <= netif->mtu - IP_HLEN);

    /* Set new offset and MF flag */
    tmp = (IP_OFFMASK & (ofo));
    if (!last || mf_set) {
      /* the last fragment has MF set if the input frame had it */
      tmp = tmp | IP_MF;
    }
    IPH_OFFSET_SET(iphdr, lwip_htons(tmp));
    IPH_LEN_SET(iphdr, lwip_htons((u16_t)(fragsize + IP_HLEN)));
    IPH_CHKSUM_SET(iphdr, 0);
#if CHECKSUM_GEN_IP
    IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_IP) {
      IPH_CHKSUM_SET(iphdr, inet_chksum(iphdr, IP_HLEN));
    }
#endif /* CHECKSUM_GEN_IP */

    /* No need for separate header pbuf - we allowed room for it in rambuf
     * when allocated.
     */
    netif->output(netif, rambuf, dest);
    IPFRAG_STATS_INC(ip_frag.xmit);

    /* Unfortunately we can't reuse rambuf - the hardware may still be
     * using the buffer. Instead we free it (and the ensuing chain) and
     * recreate it next time round the loop. If we're lucky the hardware
     * will have already sent the packet, the free will really free, and
     * there will be zero memory penalty.
     */

    pbuf_free(rambuf);
    left = (u16_t)(left - fragsize);
    ofo = (u16_t)(ofo + nfb);
  }
  MIB2_STATS_INC(mib2.ipfragoks);
  return ERR_OK;
memerr:
  MIB2_STATS_INC(mib2.ipfragfails);
  return ERR_MEM;
}
#endif /* IP_FRAG */

#endif /* LWIP_IPV4 */
