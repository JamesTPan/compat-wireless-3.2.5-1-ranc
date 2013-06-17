/**
 *
 * Communications and Networking Lab
 * National Tsing Hua University
 *
 * Author:	                James Tsunghsiao Pan (100062587)
 * Coding Language:             C
 * date:                        October 17, 2012
 * purpose:                     Gauss-Jordan Elimination
 *				for mac80211 module use.
 *
 */

#include <linux/ieee80211.h>
#include "decode.h"
#include "ieee80211_i.h"

struct matrix_ele* matrix_alloc(u8 size)
{
	struct matrix_ele *pt;
	
	pt = (struct matrix_ele *) kmalloc(sizeof(struct matrix_ele), GFP_ATOMIC);
	pt->matrix = (uint8_t *) kmalloc(sizeof(uint8_t)*(size*2)*size, GFP_ATOMIC);
	pt->flag = (bool *) kmalloc(sizeof(bool)*size, GFP_ATOMIC);
	pt->entry = (struct sk_buff **) kmalloc(sizeof(struct sk_buff *)*size, GFP_ATOMIC);
	pt->num = 0;
	pt->size = size;
	pt->rx = (struct ieee80211_rx_data *) kmalloc(sizeof(struct ieee80211_rx_data), GFP_ATOMIC);

	return pt;
}

void matrix_init(struct matrix_ele* pt)
{
	u8 i;

	memset(pt->matrix, 0, sizeof(uint8_t)*(pt->size*2)*pt->size);

	for(i = 0; i < pt->size; i++){
		*(pt->matrix + (2*i+1)*pt->size + i) = 1;
		*(pt->flag+i) = false;
		*(pt->entry+i) = NULL;
	}

	pt->dev = NULL;
	pt->next = NULL;
}

int matrix_add(struct matrix_ele* pt, u8 seq, struct sk_buff *data)
{
	/*
	 *        1      size     size   len
	 *	[flag |     matrix    | pt->entry]
	 *	[1/0  | entry  | tmp  | data     ]
	 */

	u8 i, j, *sid, sidl, leader = seq, *tmp = NULL, *entry = NULL;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr*) data->data;
	struct coding_header *chdr;
	bool set = false;
	int ret = DECODE_OK;

	//if(matrix_decodable(pt)) goto out;

	tmp = (uint8_t *) kmalloc(sizeof(uint8_t)*pt->size, GFP_ATOMIC);
	entry = (u8 *) kmalloc(sizeof(u8)*pt->size, GFP_ATOMIC);

	memset(tmp, 0, sizeof(uint8_t)*pt->size);
	memset(entry, 0, sizeof(u8)*pt->size);
	if(ieee80211_is_coding_coding(hdr->frame_control)){
		for(i = 0; i < pt->size; i++){
			*(entry+i) = leader;
			leader = GFmul(leader, seq);
		}
	} else if(ieee80211_is_coding_doucoding(hdr->frame_control)){
		chdr = (struct coding_header *) (data->data+30);
		if(chdr->more_data_len == 0){
			printk(KERN_DEBUG "[mac80211] DECODE: No data in double coding pkt!\n");
			goto fspace;
		}

		for(i = 0; i < chdr->more_data_len; i++){
			//printk(KERN_DEBUG "leader=%X (%d/%d)", leader, i, chdr->batch_size);
			sid = (u8*) (data->data+34+i);
			sidl = (u8) *sid;
			for(j = 0; j < pt->size; j++){
				//printk(KERN_DEBUG "sidl=%X", sidl);
				*(entry+j) = GFadd(*(entry+j),
						   GFmul(leader, sidl));
				sidl = GFmul(sidl, (u8) *sid);
			}
			leader = GFmul(leader, seq);
		}

		//goto fspace;
	} else{
		printk(KERN_DEBUG "[mac80211] DECODE: Error!");
		kfree(tmp);
		kfree(entry);
		dev_kfree_skb(data);

		return DECODE_NO;
	}

	for(i = 0, set = false; i < pt->size; i++){
		if((*(pt->flag+i)) == false && (*(entry+i)) != 0){
			set = true;
			*(pt->flag+i) = true;
			pt->num++;
			*(pt->entry+i) = data;			

			for(j = 0; j < pt->size; j++){
				*(pt->matrix+i*2*pt->size+j) = *(entry+j);
				*(pt->matrix+(i*2+1)*pt->size+j) 
					= GFadd(*(tmp+j), 
						*(pt->matrix+(i*2+1)*pt->size+j));
			}

			leader = *(pt->matrix+i*2*pt->size+i);
			if(leader != 0 && leader != 1)
				for(j = i; j < pt->size*2; j++)
					*(pt->matrix+i*2*pt->size+j) 
						= GFdiv(*(pt->matrix+i*2*pt->size+j), leader);

			goto fspace;
		} else if(*(pt->flag+i) && *(entry+i) != 0){
			leader = *(entry+i);
			for(j = i; j < pt->size; j++){
				*(entry+j) = GFsub(*(entry+j), 
						   GFmul(leader, *(pt->matrix+pt->size*2*i+j)));
			}
			for(j = 0; j < pt->size; j++){
                                *(tmp+j) = GFsub(*(tmp+j), 
                                               GFmul(leader, *(pt->matrix+pt->size*(2*i+1)+j)));
                        }

		}
	}

fspace:
	kfree(tmp);
	kfree(entry);
	
	//if(!matrix_decodable(pt)){
	//	if(!set && ieee80211_is_coding_doucoding(hdr->frame_control)) ret = DECODE_RD;
	//	else ret = DECODE_NO;
	//}

//out:
	if(set == false) {
		if(ieee80211_is_coding_doucoding(hdr->frame_control)) {
			//printk(KERN_DEBUG "[X] drop d");
			ret = DECODE_RD;
		}
		else{ 
			//printk(KERN_DEBUG "[X] drop c"); 
			ret = DECODE_NO;
		}
		
		dev_kfree_skb(data);
	} else {
		//if(ieee80211_is_coding_doucoding(hdr->frame_control)) 
		//	printk(KERN_DEBUG "[X] col d");
		//else 
		//	printk(KERN_DEBUG "[X] col c");
		
		if(matrix_decodable(pt) == false) ret = DECODE_NO;
	}

	//if(ret == DECODE_RD) printk(KERN_DEBUG "RD"); 
	//else if(ret == DECODE_NO) printk(KERN_DEBUG "NO"); 
	//else printk(KERN_DEBUG "OK");

	return ret;
}

bool matrix_getInverse(struct matrix_ele *pt)
{
	u8 i, j, k, leader;

	if(!matrix_decodable(pt)) return false;

	for(i = pt->size-1; i > 0 ; i--)
		for(j = 0; j < i; j++){
			leader = *(pt->matrix+j*2*pt->size+i);
			for(k = i; k < pt->size*2; k++){
				*(pt->matrix+j*2*pt->size+k) = GFsub(*(pt->matrix+j*2*pt->size+k),
								     GFmul(leader, 
									   *(pt->matrix+i*2*pt->size+k)
								     )
							       );
			}
		}

	return true;
}

struct sk_buff_head * matrix_decode(struct matrix_ele* pt)
{
	uint8_t *space, data;
	__le16 *frame_len;
	int i, j, k;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)((*(pt->entry))->data);
	struct coding_header *chdr;
	u8 more;
	int len, more_len;
	// remember to free these
	struct sk_buff *skb;
	struct sk_buff_head *seg 
		= (struct sk_buff_head *) kmalloc(sizeof(struct sk_buff_head), GFP_KERNEL);

	if(likely(ieee80211_is_coding_coding(hdr->frame_control)))
		chdr = (struct coding_header *)((*(pt->entry))->data+24);
	else
		chdr = (struct coding_header *)((*(pt->entry))->data+30);
	more = chdr->more_data_len;
	
	skb_queue_head_init(seg);

	if(likely(ieee80211_is_coding_coding(hdr->frame_control)))
		len = ((*(pt->entry))->tail) - ((*(pt->entry))->data) - 28 - more*6;
	else if(ieee80211_is_coding_doucoding(hdr->frame_control))
		len = ((*(pt->entry))->tail) - ((*(pt->entry))->data) - 34 - more;
	else{
		printk(KERN_DEBUG "[mac80211] CODING: unexpected frame\n");
		len = 0;
	}

	for(i = 0; i < pt->size; i++){
		//skb = alloc_skb(24 + len, GFP_ATOMIC); // mac header + payload (length header removed)
		space = (uint8_t *) kmalloc(sizeof(uint8_t)*len, GFP_KERNEL);
		memset(space, 0, sizeof(uint8_t)*len);
		
		// for every coding packets
		for(j = 0; j < pt->size; j++){
			hdr = (struct ieee80211_hdr *)((*(pt->entry+j))->data);
			if(likely(ieee80211_is_coding_coding(hdr->frame_control))){
				chdr = (struct coding_header *)((*(pt->entry+j))->data+24);
				more = chdr->more_data_len;
				more_len = more*6;
			} else if(ieee80211_is_coding_doucoding(hdr->frame_control)){
				chdr = (struct coding_header *)((*(pt->entry+j))->data+30);
				more = chdr->more_data_len;
				more_len = more + 6; // 6 for addr4
			} else{
				printk(KERN_DEBUG "[mac80211] CODING: unexpected frame\n");
				more_len = 0;
			}

			for(k = 0; k < len; k++){ // for every bytes
				data = GFadd(GFmul(*(pt->matrix+pt->size*(2*i+1)+j), 
						   (uint8_t) *((*(pt->entry+j))->data+28+more_len+k)),
					     *(space+k)
					    );
				memcpy(space+k, &data, sizeof(uint8_t));
			}
			
		}

		hdr = (struct ieee80211_hdr *) (*(pt->entry+i))->data;
		frame_len = (__le16*) space;

		skb = alloc_skb(24 + *frame_len, 
				GFP_KERNEL); // mac header + payload (length header removed)

		memcpy(skb_put(skb, 24), hdr, 24);
		memcpy(skb_put(skb, *frame_len), (space+2), *frame_len);

		hdr = (struct ieee80211_hdr *) skb->data;
		hdr->frame_control &= cpu_to_le16(~IEEE80211_FCTL_FTYPE);
		hdr->frame_control |= cpu_to_le16(IEEE80211_FTYPE_DATA);
		hdr->frame_control &= cpu_to_le16(~IEEE80211_FCTL_STYPE);
		hdr->frame_control |= cpu_to_le16(IEEE80211_STYPE_DATA);

		skb_queue_tail(seg, skb);
		kfree(space);
	}
	
	return seg;
}
