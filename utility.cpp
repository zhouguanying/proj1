#include "utility.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>

DataChunk::DataChunk() 
{
	m_data = 0;
	m_size = 0;
	m_readpos = 0;
	m_writepos = 0;
	m_freespace = 0;
}

DataChunk::~DataChunk()
{
	release();
}

void DataChunk::allocate(size_t size)
{
	assert(size > 0);
	
	m_data = new unsigned char [size];
	m_freespace = m_size = size;
	m_readpos = m_writepos = 0;
}

void DataChunk::data_clear()
{
	m_writepos = m_readpos = 0;
	m_freespace = m_size;
}

void DataChunk::release()
{
	if (m_data)
	{
		delete [] m_data;
		m_data = 0;
	}
}

size_t DataChunk::push_back(unsigned char *data, size_t size)
{
	size_t s = size;
	if (m_freespace < s) 
		s = m_freespace;

	if (m_writepos + s > m_size)
	{
		int c = m_size - m_writepos;
		memcpy (m_data + m_writepos, data, c);
		memcpy (m_data, data + c, s - c);
	}
	else
		memcpy (m_data + m_writepos, data, s);

	m_writepos += s;
	m_writepos %= m_size;
	m_freespace -= s;

	return s;
}

size_t DataChunk::pop_front(unsigned char *rcv, size_t size)
{
	size_t s = size;
	if (s > m_size - m_freespace)
		s = m_size - m_freespace;

	if (m_readpos + s > m_size)
	{
		int c = m_size - m_readpos;
		memcpy (rcv, m_data + m_readpos, c);
		memcpy(rcv + c, m_data, s - c);
	}
	else
		memcpy(rcv, m_data + m_readpos, s);

	m_readpos += s;
	m_readpos %= m_size;
	m_freespace += s;
	return s;
}