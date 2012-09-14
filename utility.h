#ifndef _UTILITY_H_
#define _UTILITY_
#include <stdlib.h>

struct DataChunk
{
	size_t m_size;
	size_t m_freespace;
	unsigned char *m_data;
	int m_readpos;
	int m_writepos;
	
	DataChunk();
	~DataChunk();
	
	void allocate(size_t size);
	void release(void);

	size_t freespace(void) {return m_freespace; }
	
	size_t push_back(unsigned char *data, size_t size);
	size_t pop_front(unsigned char *rcv,  size_t size);
	
	void data_clear(void);
};

#endif