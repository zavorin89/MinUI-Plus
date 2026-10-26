#ifndef RA_CHD_H
#define RA_CHD_H

// a disc reader for rcheevos' hashing that also reads .chd (via libchdr), everything else goes to rcheevos' own
struct rc_hash_cdreader;
void RA_getDiscReader(struct rc_hash_cdreader* reader);

#endif
