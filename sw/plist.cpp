////////////////////////////////////////////////////////////////////////////////
//
// Filename: 	plist.cpp
//
// Project:	AutoFPGA, a utility for composing FPGA designs from peripherals
//
// Purpose:	A PLIST is a list of peripherals.  This file defines the methods
//		associated with such a list.
//
//
//
// Creator:	Dan Gisselquist, Ph.D.
//		Gisselquist Technology, LLC
//
////////////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2017, Gisselquist Technology, LLC
//
// This program is free software (firmware): you can redistribute it and/or
// modify it under the terms of  the GNU General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTIBILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program.  (It's in the $(ROOT)/doc directory.  Run make with no
// target there if the PDF file isn't present.)  If not, see
// <http://www.gnu.org/licenses/> for a copy.
//
// License:	GPL, v3, as defined and found on www.gnu.org,
//		http://www.gnu.org/licenses/gpl.html
//
//
////////////////////////////////////////////////////////////////////////////////
//
//
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>

#include "parser.h"
#include "keys.h"
#include "kveval.h"
#include "plist.h"
#include "bitlib.h"

extern	int	gbl_err;
extern	FILE	*gbl_dump;

PLIST	plist, slist, dlist, mlist;



extern	bool	isperipheral(MAPT &pmap);
extern	bool	isperipheral(MAPDHASH &phash);

//
// Count the number of peripherals that are children of this top level hash,
// and count them by type as well.
//
int count_peripherals(MAPDHASH &info) {
	MAPDHASH::iterator	kvpair, kvptype;
	kvpair = info.find(KYNP);
	if ((kvpair != info.end())&&((*kvpair).second.m_typ == MAPT_INT)) {
		return (*kvpair).second.u.m_v;
	}

	unsigned	count = 0, np_single=0, np_double=0, np_memory=0;
	for(kvpair = info.begin(); kvpair != info.end(); kvpair++) {
		STRINGP	strp;
		if (isperipheral(kvpair->second)) {
			count++;

			// Let see what type of peripheral this is
			strp = getstring(kvpair->second, KYPTYPE);
			if (KYSINGLE == *strp) {
				np_single++;
			} else if (KYDOUBLE == *strp) {
				np_double++;
			} else if (KYMEMORY == *strp)
				np_memory++;
		}
	}

	setvalue(info, KYNP, count);
	setvalue(info, KYNPSINGLE, np_single);
	setvalue(info, KYNPDOUBLE, np_double);
	setvalue(info, KYNPMEMORY, np_memory);

	return count;
}

//
// compare_naddr
//
// This is part of the peripheral sorting mechanism, whereby peripherals are
// sorted by the numbers of addresses they use.  Peripherals using fewer
// addresses are placed first, with peripherals using more addresses placed
// later.
//
bool	compare_naddr(PERIPHP a, PERIPHP b) {
	if (!a)
		return (b)?false:true;
	else if (!b)
		return true;
	if (a->p_naddr != b->p_naddr)
		return (a->p_naddr < b->p_naddr);
	// Otherwise ... the two address types are equal.
	if ((a->p_name)&&(b->p_name))
		return (*a->p_name) < (*b->p_name);
	else if (!a->p_name)
		return true;
	else
		return true;
}

bool	compare_address(PERIPHP a, PERIPHP b) {
	if (!a)
		return (b)?false:true;
	else if (!b)
		return true;
	return (a->p_base < b->p_base);
}

//
// Add a peripheral to a given list of peripherals
int	addto_plist(PLIST &plist, MAPDHASH *phash) {
	STRINGP	pname;
	int	naddr;

	pname  = getstring(*phash, KYPREFIX);
	if (!pname) {
		fprintf(stderr, "ERR: Skipping unnamed peripheral\n");
		return -1;
	}

	if (!getvalue(*phash, KYNADDR, naddr)) {
		fprintf(stderr,
		"WARNING: Skipping peripheral %s with no addresses\n",
			pname->c_str());
		return -1;
	}
	PERIPHP p = new PERIPH;
	p->p_base = 0;
	p->p_naddr = naddr;
	p->p_awid  = nextlg(p->p_naddr)+2;
	p->p_phash = phash;
	p->p_name  = pname;

	plist.push_back(p);
	return plist.size()-1;
}
int	addto_plist(PLIST &plist, PERIPHP p) {
	plist.push_back(p);
	return plist.size()-1;
}

/*
 * build_skiplist
 *
 * Prior to any address decoding, we can build a skip list.  This collects
 * all of the relevant bits necessary for peripheral address decoding among
 * all of the peripherals within the list.  The result is that address decoding
 * can take place with fewer bits that need to be checked.  The other result,
 * though, is that a peripheral may have many locations in the memory space.
 */
void	buildskip_plist(PLIST &plist, unsigned nulladdr) {
	// Adjust the mask to find only those bits that are needed
	unsigned	masteraddr = 0;
	unsigned	adrmask = 0xffffffff,
			nullmsk = (nulladdr)?(-1<<(nextlg(nulladdr)-2)):0,
			setbits = 0,
			disbits = 0;

	// Find all the bits that our address decoder depends upon here.
	//
	// Do our computation in words, not bytes
	for(int bit=31; bit >= 0; bit--) {
		unsigned	bmsk = (1<<bit), bvl;
		bool	bset = false, onebit=true;

		for(unsigned i=0; i<plist.size(); i++) {
			if (bmsk & plist[i]->p_mask) {
				unsigned basew = plist[i]->p_base>>2;
				if (!bset) {
					// If we haven't seen this bit before,
					// let's mark it as known, and mark down
					// what it is.
					bvl = bmsk & basew;
					bset = true;
					onebit = true;
				} else if ((basew & bmsk)!=bvl) {
					// If we have seen this bit before,
					// AND it disagrees with the last time
					// we saw it, then this bit is an
					// important part of the address that
					// we cannot remove.
					onebit = false;
					break;
				}
			}
		}
		if (bset)
			setbits |= bmsk;
		if (!onebit) {
			disbits |= bmsk;
			continue;
		}
		// If its not part of anyone's mask, ... don't use it
		//
		// Not appropriate.  What if one peripheral requires 11 bit
		// decoding, but no one else does?  i.e. one peripheral has a
		// 4 word address space, but all others have an 8 word or more
		// address space?
		// if (!bset)
			// continue;

		if (bmsk & nullmsk) {
			if ((bset)&&(bvl != 0))
				continue;
		}

		// The adrmask is a mask of bits necessary for selection
		adrmask &= (~bmsk);
		// The masteraddr is a mask of what the bits are that are
		// to be set.
		masteraddr|= bvl;
	}

	adrmask<<=2; // Convert the result from to octets
	if (gbl_dump)
		fprintf(gbl_dump, "ADRMASK = %08x (%2d bits)\n", adrmask, popc(adrmask));
	// printf("MASTRAD = %08x\n", masteraddr);
	// printf("DISBITS = %08x\n", disbits);
	// printf("SETBITS = %08x\n", setbits);

	// adrmask in octets
	int sbaw = popc(adrmask);
	for(unsigned i=0; i<plist.size(); i++) {
		// Do this work in octet space
		unsigned skipaddr = 0, skipmask = 0, skipnbits = 0,
			pmsk = plist[i]->p_mask<<2; // Mask in octets
		PERIPHP	p = plist[i];
		for(int bit=31; bit>= 0; bit--) {
			unsigned	bmsk = (1<<bit);
			if (bmsk & adrmask) {
				skipaddr <<= 1;
				skipmask = (skipmask<<1)|((pmsk&bmsk)?1:0);
				if (bmsk & pmsk)
					skipnbits ++;
				if (p->p_base & bmsk)	// Octets
					skipaddr |= 1;
			}
		}

		p->p_skipaddr = skipaddr;
		p->p_skipmask = skipmask;
		p->p_sadrmask = adrmask;
		p->p_skipnbits= sbaw;
		//
		setvalue(*plist[i]->p_phash, KYSKIPADDR,  skipaddr);
		setvalue(*plist[i]->p_phash, KYSKIPMASK,  skipmask);
		setvalue(*plist[i]->p_phash, KYSKIPDEFN,  adrmask);
		setvalue(*plist[i]->p_phash, KYSKIPNBITS, skipnbits);
		setvalue(*plist[i]->p_phash, KYSKIPAWID,  sbaw);
		// printf("// %08x %08x \n", p->p_base, pmsk);
		if (gbl_dump)
			fprintf(gbl_dump,
			"// skip-assigning %12s_... to %08x & %08x, "
				"%2d, [%08x]AWID=%d\n",
			plist[i]->p_name->c_str(), skipaddr, skipmask,
			skipnbits, adrmask, sbaw);
	}

	/* Just to be sure, let's test that this works ... */
	for(unsigned i=0; i<plist.size(); i++) {
		// printf("%2d: %2s\n", i, plist[i]->p_name->c_str());
		assert((plist[i]->p_skipaddr & (~plist[i]->p_skipmask))==0);
	}
	for(unsigned a=0; a<(1u<<sbaw); a++) {
		int nps = 0;
		for(unsigned i=0; i<plist.size(); i++) {
			if ((a&plist[i]->p_skipmask)==plist[i]->p_skipaddr)
				nps++;
		} assert(nps < 2);
	}
}

/*
 * build_plist
 *
 * Collect our peripherals into one of four lists.  This allows us to have
 * ordered access through the peripherals later.
 */
void	build_plist(MAPDHASH &info) {
	MAPDHASH::iterator	kvpair;

	int np = count_peripherals(info);

	if (np < 1) {
		if (gbl_dump)
			fprintf(gbl_dump, "Only %d peripherals\n", np);
		return;
	}

	for(kvpair = info.begin(); kvpair != info.end(); kvpair++) {
		if (isperipheral(kvpair->second)) {
			STRINGP	ptype;
			MAPDHASH	*phash = kvpair->second.u.m_m;
			if (NULL != (ptype = getstring(kvpair->second, KYPTYPE))) {
				if (KYSINGLE == *ptype)
					addto_plist(slist, phash);
				else if (KYDOUBLE == *ptype)
					addto_plist(dlist, phash);
				else if (KYMEMORY == *ptype) {
					addto_plist(plist, phash);
					addto_plist(mlist, plist[plist.size()-1]);
				} else
					addto_plist(plist, phash);
			} else
				addto_plist(plist, phash);
		}
	}

	// Sort by address usage
	std::sort(slist.begin(), slist.end(), compare_naddr);
	std::sort(dlist.begin(), dlist.end(), compare_naddr);
	std::sort(mlist.begin(), mlist.end(), compare_naddr);
	std::sort(plist.begin(), plist.end(), compare_naddr);
}
