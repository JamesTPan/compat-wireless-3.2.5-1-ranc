/** 
 * Communications and Networking Lab
 * National Tsing Hua University
 *
 * Original Auther:            	Chieh-Hao Chang
 * Original Coding Language:    C++
 *
 * Modify by 			James Tsunghsiao Pan (100062587)
 * Coding Language: 		C
 * date: 			April 30, 2012
 * purpose: 			for mac80211 module use.
 *
 */

#include "GaloisField.h"

// member
static uint8_t          GF[256];
static uint8_t          inv_GF[256];

// fuctions of Galois Field
// add
/*uint8_t GFadd(uint8_t a,uint8_t b){
        return a ^ b;
}

// subtract
uint8_t GFsub(uint8_t a,uint8_t b){
        return a ^ b;
}
*/
// multiply
uint8_t GFmul(uint8_t a,uint8_t b){
        if(a == 0 || b == 0)
                return 0;
        else
                return GF[ (inv_GF[a] + inv_GF[b]) % 255 ];
}

// divide
uint8_t GFdiv(uint8_t a,uint8_t b){
        if(unlikely(a == 0 || b == 0))
                return 0;
        else
                return GF[(inv_GF[a] - inv_GF[b] + 255) % 255];
}

// Galois Field initialize
void GaloisField_init(void) {
        int i = 0;

        GF[0]           = 1;
        inv_GF[1]       = 0;

        for(i = 1; i < 255; i++)
        {
                if(GF[i - 1] >= 128)
                        GF[i] = GFadd(GF[i - 1] << 1, pivot);
                else
                        GF[i] = GF[i - 1] << 1;

                inv_GF[GF[i]] = i;
        }
	GF[255] = 1;
	inv_GF[1] = 0xff;
}

