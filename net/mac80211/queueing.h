/**
 * Communications and Networking Lab
 * National Tsing Hua University
 *
 * Auther 					James Tsunghsiao Pan (100062587)
 * purpose: 					queue and timer elements for network coding usage 
 *
 */

#ifndef QUEUEING_H
#define QUEUEING_H

#include <linux/skbuff.h>
#include <linux/timer.h>
#include "decode.h"

#define BATCH_SIZE		15
#define BATCH_DELAY		5
#define LENGTH_THRSH		1000
#define BATCH_TX_THRESH		7
#define RELAY_THRESH		1000
#define RELAY_BASIC		250
#define INT_WORK_RATE_INCR	(int) (((double) 6/BATCH_SIZE)*RELAY_THRESH)
#define MAX_WORK_RATE		2000
#define MIN_WORK_RATE		125
#define CLEANUP_THRESH		60
#define FB_CYCLE		10

struct batch_schedule{
        struct batch_schedule *prev;
	struct batch_schedule *next;
        unsigned long data;
};

struct queue_ele{
	struct sk_buff_head qhead;
	struct sk_buff_head batch;
	struct ieee80211_sub_if_data *sdata;
	struct timer_list timer;
	struct timer_list c_timer;
	bool finished;
	bool timer_ex;
	bool quick_tx;
	u8 dst[6];
	u8 src[6];
	u8 id;
	u8 seq;
	unsigned long t_stamp;
	int tx_number;
	int add_number;
	int inc_number;
	unsigned int max_size;
	struct matrix_ele *matrix;
	
	// TODO: implement a double linked list structure
	// notice the initialization issue
	struct queue_ele *next;
	struct batch_schedule addr;
};

static inline void remove_queue(struct queue_ele **head, struct queue_ele *ele){
	struct queue_ele *pt = *head, *tmp;
	for(; pt != NULL; pt = pt->next){
		if(pt == ele){
			if(ele->addr.next != NULL || ele->addr.prev != NULL)
				printk(KERN_DEBUG "[mac80211] QUEUE: ERROR!");

			del_timer(&pt->timer);
			skb_queue_purge(&ele->qhead);
			skb_queue_purge(&ele->batch);
			if(ele->matrix != NULL) matrix_free(ele->matrix);
			ele->matrix = NULL;
			
			if(*head == ele){
				*head = ele->next;
			} else {
				for(tmp = *head; tmp != NULL; tmp = tmp->next)
					if(tmp->next == ele){
						tmp->next = ele->next;
						break;
					}
			}
			
			kfree(ele);
			break;
		}
	}
}

#endif /* QUEUEING*/
