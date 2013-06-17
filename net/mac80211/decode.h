/**
 *
 * Communications and Networking Lab
 * National Tsing Hua University
 *
 * Auther:	                James Tsunghsiao Pan (100062587)
 * Coding Language:             C
 * date:                        October 17, 2012
 * purpose:                     Gauss-Jordan Elimination
 *				for mac80211 module use.
 *
 */


#ifndef DECODE_H
#define DECODE_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include "GaloisField.h"

#define DECODE_NO	0
#define DECODE_OK	1
#define DECODE_RD	2

struct matrix_ele{
	uint8_t *matrix;
	bool *flag;
	struct sk_buff **entry;
	u8 num;
	u8 size;
	struct matrix_ele *next;
	struct ieee80211_rx_data *rx;
	struct net_device *dev;
};

struct matrix_ele* matrix_alloc(u8 size);

static inline void matrix_free(struct matrix_ele* pt){
	u8 i;

	for(i = 0; i < pt->size; i++)
		if(*(pt->entry+i) != NULL) kfree_skb(*(pt->entry+i));	

	kfree(pt->matrix);
	kfree(pt->flag);
	kfree(pt->entry);
	if(pt->rx != NULL) kfree(pt->rx);
	pt->dev = NULL;
	kfree(pt);
}

static inline bool matrix_decodable(struct matrix_ele* pt){
	return pt->num == pt->size;
}

void matrix_init(struct matrix_ele* pt);
int matrix_add(struct matrix_ele* pt, u8 entry, struct sk_buff *data);
struct sk_buff_head * matrix_decode(struct matrix_ele* pt);
bool matrix_getInverse(struct matrix_ele *pt);
#endif
