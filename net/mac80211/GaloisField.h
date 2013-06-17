#ifndef GALOIS_FIELD_H
#define GALOIS_FIELD_H

#include <linux/types.h>

/**
 *
 * Communications and Networking Lab
 * National Tsing Hua University
 *
 * Original Auther: 		Chieh-Hao Chang
 * Original Coding Language: 	C++
 *
 * Modify by 			James Tsunghsiao Pan (100062587)
 * Coding Language: 		C
 * date: 			April 30, 2012
 * purpose: 			for mac80211 module use.
 *
 */

// define type of value of GF
typedef struct uint8_t GFV;

static const uint8_t 	pivot = 29;	

// initialize the Galois Feild
// This function must be called before use the structure of Galois Field
void GaloisField_init(void);

#define GFadd(a,b) ((a)^(b))
#define GFsub(a,b) ((a)^(b))

//uint8_t GFadd(uint8_t a,uint8_t b);
//uint8_t GFsub(uint8_t a,uint8_t b);
uint8_t GFmul(uint8_t a,uint8_t b);
uint8_t GFdiv(uint8_t a,uint8_t b);

#endif
