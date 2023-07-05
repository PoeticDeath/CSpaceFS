#include <windows.h>
#include <stdio.h>
#include <iostream>
#include <map>
#include <time.h>
#include <string>
#include "SpaceFS.h"

static inline unsigned upperchar(unsigned c)
{
	/*
	 * Bit-twiddling upper case char:
	 *
	 * - Let signbit(x) = x & 0x100 (treat bit 0x100 as "signbit").
	 * - 'A' <= c && c <= 'Z' <=> s = signbit(c - 'A') ^ signbit(c - ('Z' + 1)) == 1
	 *     - c >= 'A' <=> c - 'A' >= 0      <=> signbit(c - 'A') = 0
	 *     - c <= 'Z' <=> c - ('Z' + 1) < 0 <=> signbit(c - ('Z' + 1)) = 1
	 * - Bit 0x20 = 0x100 >> 3 toggles uppercase to lowercase and vice-versa.
	 *
	 * This is actually faster than `(c - 'a' <= 'z' - 'a') ? (c & ~0x20) : c`, even
	 * when compiled using cmov conditional moves at least on this system (i7-1065G7).
	 *
	 * See https://godbolt.org/z/ebv131Wrh
	 */
	unsigned s = ((c - 'a') ^ (c - ('z' + 1))) & 0x100;
	return c & ~(s >> 3);
}

static inline int wcsincmp(const wchar_t* s0, const wchar_t* t0, int n)
{
	/* Use fast loop for ASCII and fall back to CompareStringW for general case. */
	const wchar_t* s = s0;
	const wchar_t* t = t0;
	int v = 0;
	for (const void* e = t + n; e > (const void*)t; ++s, ++t)
	{
		unsigned sc = *s, tc = *t;
		if (0xffffff80 & (sc | tc))
		{
			v = CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, s0, n, t0, n);
			if (0 != v)
			{
				return v - 2;
			}
			else
			{
				return _wcsnicmp(s, t, n);
			}
		}
		if (0 != (v = upperchar(sc) - upperchar(tc)) || !tc)
		{
			break;
		}
	}
	return v;/*(0 < v) - (0 > v);*/
}

void encode(std::map<unsigned, unsigned> emap, char*& str, unsigned long long& len)
{
	if (len % 2)
	{
		len++;
		char* alc = (char*)realloc(str, len);
		if (!alc)
		{
			return;
		}
		str = alc;
		alc = NULL;
		str[len - 1] = 32;
		str[len - 2] = 46;
	}
	char* bytes = (char*)calloc(len / 2 + 1, 1);
	if (!bytes)
	{
		return;
	}
	for (unsigned long long i = 0; i < len; i += 2)
	{
		bytes[i / 2] = emap[str[i] << 8 | str[i + 1]];
	}
	char* alc = (char*)realloc(str, len / 2 + 1);
	if (!alc)
	{
		free(bytes);
		return;
	}
	str = alc;
	memcpy(str, bytes, len / 2);
	free(bytes);
}

void decode(std::map<unsigned, unsigned> dmap, char*& bytes, unsigned long long len)
{
	char* str = (char*)calloc(len * 2, 1);
	if (!str)
	{
		return;
	}
	unsigned d;
	for (unsigned long long i = 0; i < len; i++)
	{
		d = dmap[bytes[i] & 0xff];
		str[i * 2] = d >> 8;
		str[i * 2 + 1] = d & 0xff;
	}
	char* alc = (char*)realloc(bytes, len * 2 + 1);
	if (!alc)
	{
		free(str);
		return;
	}
	bytes = alc;
	memcpy(bytes, str, len * 2);
	free(str);
}

void cleantablestr(char* charmap, char*& tablestr)
{
	unsigned long long i = 0;
	unsigned b = 0;
	for (; i < strlen(tablestr); i++)
	{
		b = 1;
		for (unsigned o = 0; o < strlen(charmap); o++)
		{
			if (tablestr[i] == charmap[o])
			{
				b = 0;
				break;
			}
		}
		if (b == 1)
		{
			break;
		}
	}
	tablestr[i] = 0;
}

int settablesize(unsigned long sectorsize, unsigned long& tablesize, unsigned long long& extratablesize, char*& table)
{
	extratablesize = (tablesize * static_cast<unsigned long long>(sectorsize));
	char* alc = NULL;
	alc = (char*)realloc(table, extratablesize);
	if (!alc)
	{
		tablesize = 1 + table[4] + (table[3] << 8) + (table[2] << 16) + (table[1] << 24);
		extratablesize = (tablesize * static_cast<unsigned long long>(sectorsize));
		return 1;
	}
	table = alc;
	tablesize--;
	table[1] = (tablesize & 0xff000000) >> 24;
	table[2] = (tablesize & 0x00ff0000) >> 16;
	table[3] = (tablesize & 0x0000ff00) >> 8;
	table[4] = (tablesize & 0x000000ff);
	return 0;
}

void resetcloc(unsigned long long& cloc, char*& cblock, unsigned long long clen, std::string& str0, std::string& str1, std::string& str2, unsigned step)
{
	std::string str = std::string(cblock + strlen(cblock) - cloc, cloc);
	switch (step)
	{
	case 0:
		str0 = str;
		break;
	case 1:
		str1 = str;
		break;
	case 2:
		str2 = str;
		break;
	}
	cloc = 0;
	free(cblock);
	cblock = (char*)calloc(clen, 1);
}

unsigned long long getpindex(unsigned long long index, char* tablestr)
{
	unsigned long long pindex = 0;
	while ((tablestr[index - pindex - 1] & 0xff) != 46)
	{
		pindex++;
		if (!(index - pindex))
		{
			break;
		}
	}
	return pindex;
}

int getfilesize(unsigned long sectorsize, unsigned long long index, char* tablestr, unsigned long long& filesize)
{
	unsigned long long pindex = getpindex(index, tablestr);
	if (!index || !pindex)
	{
		filesize = 0;
		return 0;
	}
	unsigned long long o = 0;
	unsigned long long clen = 256;
	char* cblock = (char*)calloc(clen, 1);
	if (!cblock)
	{
		return 1;
	}
	char* alc = NULL;
	unsigned long long cloc = 0;
	std::string str0;
	std::string str1;
	std::string str2;
	std::string rstr;
	unsigned step = 0;
	unsigned range = 0;
	for (unsigned long long i = 0; i < pindex + 1; i++)
	{
		switch (tablestr[index - pindex + i] & 0xff)
		{
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (step)
			{
				o += std::stoull(str2, 0, 10) - std::stoull(str1, 0, 10);
			}
			else if (!range)
			{
				o += sectorsize;
			}
			if (range)
			{
				o += (sectorsize * (std::stoull(str0, 0, 10) - std::stoull(rstr, 0, 10) + 1));
			}
			step = 0;
			range = 0;
			break;
		case 45: //-
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step = 0;
			range++;
			rstr = str0;
			break;
		case 44: //,
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (step)
			{
				o += std::stoull(str2, 0, 10) - std::stoull(str1, 0, 10);
			}
			else if (!range)
			{
				o += sectorsize;
			}
			if (range)
			{
				o += (sectorsize * (std::stoull(str0, 0, 10) - std::stoull(rstr, 0, 10) + 1));
			}
			step = 0;
			range = 0;
			break;
		default: //0-9
			if (cloc > clen - 2)
			{
				clen += 256;
				alc = (char*)realloc(cblock, clen);
				if (!alc)
				{
					free(cblock);
					return 1;
				}
				cblock = alc;
				alc = NULL;
			}
			cblock[cloc] = tablestr[index - pindex + i];
			cloc++;
			break;
		}
	}
	free(cblock);
	filesize = o;
	return 0;
}

void addtopartlist(unsigned long sectorsize, unsigned range, unsigned step, std::string str0, std::string str1, std::string str2, std::string rstr, std::map<std::string, std::map<unsigned long, unsigned long long>>& partlist, std::map<std::string, unsigned long>& list, unsigned long long& usedblocks)
{
	if (str0 == "")
	{
		return;
	}
	if (!range)
	{
		if (!step)
		{
			list[str0] = 0;
			usedblocks++;
			//std::cout << str0 << std::endl;
		}
	}
	else
	{
		for (unsigned long long i = std::strtoull(rstr.c_str(), 0, 10); i < std::strtoull(str0.c_str(), 0, 10) + 1; i++)
		{
			list[std::to_string(i)] = 0;
			usedblocks++;
		}
		//std::cout << rstr << "-" << str0 << ",";
	}
	if (step)
	{
		for (unsigned long i = std::strtoul(str1.c_str(), 0, 10); i < std::strtoul(str2.c_str(), 0, 10); i++)
		{
			if (i / 64 < std::strtoul(str2.c_str(), 0, 10) / 64)
			{
				partlist[str0][i / 64] |= 0xffffffffffffffff << i % 64;
				if (list[str0] == sectorsize)
				{
					usedblocks++;
				}
				list[str0] -= 64 - i % 64;
				i += 63 - i % 64;
			}
			else
			{
				partlist[str0][i / 64] |= static_cast<unsigned long long>(1) << i % 64;
				if (list[str0] == sectorsize)
				{
					usedblocks++;
				}
				list[str0]--;
			}
		}
		//std::cout << str0 << ";" << str1 << ";" << str2 << std::endl;
	}
}

int findblock(unsigned long sectorsize, unsigned long long disksize, unsigned long tablesize, char* tablestr, char*& block, unsigned long long& blockstrlen, unsigned long blocksize, unsigned long long& usedblocks)
{
	usedblocks = 0;
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++)
	{
		if ((tablestr[i] & 0xff) == 46)
		{
			tablelen = i + 1;
		}
	}
	unsigned long long clen = 256;
	char* cblock = (char*)calloc(clen, 1);
	char* alc = NULL;
	unsigned long long cloc = 0;
	std::string str0;
	std::string str1;
	std::string str2;
	std::string rstr;
	unsigned step = 0;
	unsigned range = 0;
	std::map<std::string, std::map<unsigned long, unsigned long long>> partlist;
	std::map<std::string, unsigned long> list;
	for (unsigned long i = 0; i < disksize / sectorsize - tablesize; i++)
	{
		list[std::to_string(i)] = sectorsize;
	}
	for (unsigned long long i = 0; i < tablelen; i++)
	{
		switch (tablestr[i] & 0xff)
		{
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			addtopartlist(sectorsize, range, step, str0, str1, str2, rstr, partlist, list, usedblocks);
			step = 0;
			range = 0;
			break;
		case 45: //-
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step = 0;
			range++;
			rstr = str0;
			break;
		case 44: //,
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			addtopartlist(sectorsize, range, step, str0, str1, str2, rstr, partlist, list, usedblocks);
			step = 0;
			range = 0;
			break;
		default: //0-9
			if (cloc > clen - 2)
			{
				clen += 256;
				alc = (char*)realloc(cblock, clen);
				if (!alc)
				{
					free(cblock);
					return 1;
				}
				cblock = alc;
				alc = NULL;
			}
			cblock[cloc] = tablestr[i];
			cloc++;
			break;
		}
	}
	free(cblock);
	unsigned long bytecount = 0;
	unsigned long o = 0;
	std::string s;
	for (unsigned long long i = 0; i < disksize / sectorsize - tablesize; i++)
	{
		if (list[std::to_string(i)] >= blocksize)
		{
			o = 0;
			if (list[std::to_string(i)] == sectorsize)
			{
				bytecount = blocksize;
			}
			while (bytecount < blocksize && o < sectorsize)
			{
				if (!(partlist[std::to_string(i)][o / 64] & static_cast<unsigned long long>(1) << o % 64))
				{
					bytecount++;
				}
				else
				{
					o++;
					bytecount = 0;
				}
				if (blocksize > sectorsize - o)
				{
					break;
				}
			}
			if (bytecount == blocksize)
			{
				if (blocksize % sectorsize)
				{
					s = std::to_string(i) + ";" + std::to_string(o) + ";" + std::to_string(o + bytecount);
				}
				else
				{
					s = std::to_string(i);
				}
				break;
			}
		}
	}
	if (s == "")
	{
		return 1;
	}
	blockstrlen = strlen(s.c_str());
	char* alc1 = (char*)realloc(block, blockstrlen + 1);
	if (!alc1)
	{
		return 1;
	}
	block = alc1;
	memcpy(block, s.c_str(), blockstrlen);
	return 0;
}

int alloc(unsigned long sectorsize, unsigned long long disksize, unsigned long tablesize, char* charmap, char*& tablestr, unsigned long long& index, unsigned long long size, unsigned long long& usedblocks)
{
	char* block = (char*)calloc(256, 1);
	unsigned long long tablestrlen = strlen(tablestr);
	unsigned long long blockstrlen = 0;
	unsigned long long o = 0;
	if ((tablestr[index - 1] & 0xff) != 46 && index)
	{
		o++;
	}
	char* alc1 = (char*)calloc(tablestrlen - index + 1, 1);
	for (unsigned long long i = 0; i < tablestrlen - index; i++)
	{
		alc1[i] = tablestr[index + i];
	}
	for (unsigned long long p = 0; p < size / sectorsize; p++)
	{
		findblock(sectorsize, disksize, tablesize, tablestr, block, blockstrlen, sectorsize, usedblocks);
		if (!block)
		{
			return 1;
		}
		char* alc2 = (char*)realloc(tablestr, tablestrlen + blockstrlen + o + strlen(alc1) + 1);
		if (!alc2)
		{
			free(alc1);
			return 1;
		}
		if (o)
		{
			alc2[index] = 44;
		}
		for (unsigned long long i = 0; i < blockstrlen; i++)
		{
			alc2[index + o + i] = block[i];
		}
		for (unsigned long long i = 0; i < strlen(alc1); i++)
		{
			alc2[index + o + blockstrlen + i] = alc1[i];
		}
		tablestr = alc2;
		alc2 = NULL;
		index += blockstrlen;
		if (o)
		{
			index++;
		}
		o = 1;
		cleantablestr(charmap, tablestr);
	}
	if (size % sectorsize)
	{
		findblock(sectorsize, disksize, tablesize, tablestr, block, blockstrlen, size % sectorsize, usedblocks);
		if (!block)
		{
			return 1;
		}
		char* alc2 = (char*)realloc(tablestr, tablestrlen + blockstrlen + o + strlen(alc1) + 1);
		if (!alc2)
		{
			free(alc1);
			return 1;
		}
		if (o)
			alc2[index] = 44;
		for (unsigned long long i = 0; i < blockstrlen; i++)
		{
			alc2[index + o + i] = block[i];
		}
		for (unsigned long long i = 0; i < strlen(alc1); i++)
		{
			alc2[index + blockstrlen + o + i] = alc1[i];
		}
		tablestr = alc2;
		alc2 = NULL;
		index += blockstrlen;
		if (o)
		{
			index++;
		}
		cleantablestr(charmap, tablestr);
	}
	free(block);
	free(alc1);
	return 0;
}

int dealloc(unsigned long sectorsize, char* charmap, char*& tablestr, unsigned long long& index, unsigned long long filesize, unsigned long long size)
{
	char* alc = NULL;
	char* alc1 = (char*)calloc(strlen(tablestr) - index, 1);
	char* alc2 = (char*)calloc(1, 1);
	if (!alc1)
	{
		return 1;
	}
	for (unsigned long long i = 0; i < strlen(tablestr) - index; i++)
	{
		alc1[i] = tablestr[index + i];
	}
	if (size % sectorsize)
	{
		unsigned long long pindex = getpindex(index, tablestr);
		alc = (char*)realloc(alc2, pindex + 1);
		if (!alc)
		{
			free(alc1);
			free(alc2);
			return 1;
		}
		alc2 = alc;
		alc = NULL;
		for (unsigned long long i = 0; i < pindex; i++)
		{
			alc2[i] = tablestr[index - pindex + i];
		}
		if (!((filesize - size) % sectorsize))
		{ // Dealloc entire block out of alc2
			for (unsigned long long i = 0; i < pindex + 1; i++)
			{
				if ((alc2[pindex - i] & 0xff) == 44)
				{
					alc2[pindex - i] = 0;
					break;
				}
				alc2[pindex - i] = 0;
			}
		}
		else if (!(filesize % sectorsize))
		{ // Realloc full block as part block
			unsigned long long alc2len = strlen(alc2);
			char part[4] = ";0;";
			for (unsigned long long i = 0; i < 3; i++)
			{
				alc2[alc2len + i] = part[i];
			}
			for (unsigned long long i = 0; i < strlen(std::to_string(sectorsize - size % sectorsize).c_str()); i++)
			{
				alc2[alc2len + 3 + i] = std::to_string(sectorsize - size % sectorsize).c_str()[i];
			}
			alc2[alc2len + 3 + strlen(std::to_string(sectorsize - size % sectorsize).c_str())] = 0;
		}
		else
		{ // Realloc part block as smaller part block
			unsigned long long alc2partlen = 0;
			for (; alc2partlen < strlen(alc2); alc2partlen++)
			{
				if ((alc2[strlen(alc2) - alc2partlen] & 0xff) == 59)
				{
					break;
				}
			}
			unsigned long alc2part = std::strtoul(alc2 + strlen(alc2) - alc2partlen + 1, 0, 10);
			for (unsigned long long i = 0; i < strlen(std::to_string(alc2part - size % sectorsize).c_str()); i++)
			{
				alc2[strlen(alc2) - alc2partlen + 1 + i] = std::to_string(alc2part - size % sectorsize)[i];
			}
			alc2[strlen(alc2) - alc2partlen + strlen(std::to_string(alc2part - size % sectorsize).c_str()) + 1] = 0;
		}
		unsigned long long off = 0;
		for (; off < strlen(alc2); off++)
		{
			if (!(alc2[off] & 0xff))
			{
				break;
			}
			tablestr[index - pindex + off] = alc2[off];
		}
		for (unsigned long long i = 0; i < strlen(alc1); i++)
		{
			tablestr[index - pindex + off + i] = alc1[i];
		}
		cleantablestr(charmap, tablestr);
		index = index - pindex + off;
		filesize -= size % sectorsize;
	}
	for (unsigned long long p = 0; p < size / sectorsize; p++)
	{ // Dealloc entire block out of alc2
		unsigned long long pindex = getpindex(index, tablestr);
		alc = (char*)realloc(alc2, pindex + 1);
		if (!alc)
		{
			free(alc1);
			free(alc2);
			return 1;
		}
		alc2 = alc;
		alc = NULL;
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++)
		{
			alc1[i] = tablestr[index + i];
		}
		for (unsigned long long i = 0; i < pindex; i++)
		{
			alc2[i] = tablestr[index - pindex + i];
		}
		for (unsigned long long i = 0; i < pindex + 1; i++)
		{
			if ((alc2[pindex - i] & 0xff) == 44)
			{
				alc2[pindex - i] = 0;
				break;
			}
			alc2[pindex - i] = 0;
		}
		unsigned long long off = 0;
		for (; off < strlen(alc2); off++)
		{
			if (!(alc2[off] & 0xff))
			{
				break;
			}
			tablestr[index - pindex + off] = alc2[off];
		}
		for (unsigned long long i = 0; i < strlen(alc1); i++)
		{
			tablestr[index - pindex + off + i] = alc1[i];
		}
		cleantablestr(charmap, tablestr);
		index = index - pindex + off;
		filesize -= sectorsize;
	}
	free(alc1);
	free(alc2);
	return 0;
}

void getfilenameindex(PWSTR filename, char* filenames, unsigned long long filenamecount, unsigned long long& filenameindex, unsigned long long& filenamestrindex)
{
	unsigned long long filenamesize = wcslen(filename);
	unsigned long long tempnamesize = 256;
	wchar_t* file = (wchar_t*)calloc(tempnamesize + 1, sizeof(wchar_t));
	if (!file)
	{
		return;
	}
	for (; filenameindex < filenamecount;)
	{
		for (unsigned long long i = 0; i < tempnamesize; i++)
		{
			if ((filenames[filenamestrindex + i] & 0xff) == 255 || (filenames[filenamestrindex + i] & 0xff) == 42)
			{
				file[i] = 0;
				filenamestrindex += static_cast<unsigned long long>(i) + 1;
				break;
			}
			file[i] = filenames[filenamestrindex + i];
			if (i > tempnamesize - 1)
			{
				tempnamesize += 256;
				wchar_t* alc = (wchar_t*)realloc(file, tempnamesize + 1);
				if (!alc)
				{
					free(file);
					return;
				}
				file = alc;
				alc = NULL;
			}
		}
		if (!wcsincmp(file, filename, filenamesize) && wcslen(file) == filenamesize)
		{
			break;
		}
		if ((filenames[filenamestrindex - 1] & 0xff) == 255)
		{
			filenameindex++;
		}
	}
	filenamestrindex--;
	free(file);
}

unsigned long long gettablestrindex(PWSTR filename, char* filenames, char* tablestr, unsigned long long filenamecount)
{
	unsigned long long filenameindex = 0;
	unsigned long long filenamestrindex = 0;
	getfilenameindex(filename, filenames, filenamecount, filenameindex, filenamestrindex);
	unsigned long long index = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++)
	{
		if ((tablestr[i] & 0xff) == 46)
		{
			if (index == filenameindex)
			{
				return i;
			}
			index++;
		}
	}
}

int desimp(char* charmap, char*& tablestr)
{
	char* newtablestr = (char*)calloc(strlen(tablestr) + 1, 1);
	if (!newtablestr)
		return 1;
	unsigned long long newloc = 0;
	unsigned long long tablelen = 0;
	unsigned long long newtablelen = strlen(tablestr);
	for (unsigned long long i = 0; i < strlen(tablestr); i++)
	{
		if ((tablestr[i] & 0xff) == 46)
		{
			tablelen = i + 1;
		}
	}
	char* alc = NULL;
	unsigned long long clen = 256;
	char* cblock = (char*)calloc(clen, 1);
	if (!cblock)
	{
		free(newtablestr);
		return 1;
	}
	unsigned long long cloc = 0;
	std::string str0;
	std::string str1;
	std::string str2;
	std::string rstr;
	unsigned step = 0;
	unsigned range = 0;
	for (unsigned long long i = 0; i < tablelen; i++)
	{
		switch (tablestr[i] & 0xff)
		{
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (!range)
			{
				for (unsigned long long i = 0; i < str0.length(); i++)
				{
					newtablestr[newloc] = str0[i];
					newloc++;
				}
			}
			else
			{
				for (unsigned long long p = std::strtoull(rstr.c_str(), 0, 10); p < std::strtoull(str0.c_str(), 0, 10) + 1; p++)
				{
					for (unsigned long long i = 0; i < std::to_string(p).length(); i++)
					{
						newtablestr[newloc] = std::to_string(p)[i];
						newloc++;
					}
					newtablestr[newloc] = 44;
					newloc++;
				}
				newloc--;
			}
			if (step)
			{
				newtablestr[newloc] = 59;
				newloc++;
				for (unsigned long long i = 0; i < str1.length(); i++)
				{
					newtablestr[newloc] = str1[i];
					newloc++;
				}
				newtablestr[newloc] = 59;
				newloc++;
				for (unsigned long long i = 0; i < str2.length(); i++)
				{
					newtablestr[newloc] = str2[i];
					newloc++;
				}
			}
			newtablestr[newloc] = 46;
			newloc++;
			step = 0;
			range = 0;
			break;
		case 45: //-
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step = 0;
			range++;
			rstr = str0;
			break;
		case 44: //,
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (!range)
			{
				for (unsigned long long i = 0; i < str0.length(); i++)
				{
					newtablestr[newloc] = str0[i];
					newloc++;
				}
			}
			else
			{
				for (unsigned long long p = std::strtoull(rstr.c_str(), 0, 10); p < std::strtoull(str0.c_str(), 0, 10) + 1; p++)
				{
					for (unsigned long long i = 0; i < std::to_string(p).length(); i++)
					{
						newtablestr[newloc] = std::to_string(p)[i];
						newloc++;
					}
					newtablestr[newloc] = 44;
					newloc++;
				}
				newloc--;
			}
			newtablestr[newloc] = 44;
			newloc++;
			step = 0;
			range = 0;
			break;
		default: //0-9
			cblock[cloc] = tablestr[i];
			cloc++;
			break;
		}
		if (cloc > clen - 2)
		{
			clen += 256;
			alc = (char*)realloc(cblock, clen);
			if (!alc)
			{
				free(cblock);
				free(newtablestr);
				return 1;
			}
			cblock = alc;
			alc = NULL;
		}
		if (i > newtablelen - 2)
		{
			newtablelen += 256;
			alc = (char*)realloc(newtablestr, newtablelen);
			if (!alc)
			{
				free(cblock);
				free(newtablestr);
				return 1;
			}
			newtablestr = alc;
			alc = NULL;
		}
	}
	free(cblock);
	char* alc1 = (char*)realloc(tablestr, strlen(newtablestr) + 1);
	if (!alc1)
	{
		free(newtablestr);
		return 1;
	}
	tablestr = alc1;
	memcpy(tablestr, newtablestr, strlen(newtablestr));
	free(newtablestr);
	cleantablestr(charmap, tablestr);
	return 0;
}

int simp(char* charmap, char*& tablestr)
{
	char* newtablestr = (char*)calloc(strlen(tablestr) + 1, 1);
	unsigned long long newloc = 0;
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++)
	{
		if ((tablestr[i] & 0xff) == 46)
		{
			tablelen = i + 1;
		}
	}
	char* alc = NULL;
	unsigned long long clen = 256;
	char* cblock = (char*)calloc(clen, 1);
	unsigned long long cloc = 0;
	std::string str0;
	std::string str1;
	std::string str2;
	std::string rstr;
	unsigned step = 0;
	for (unsigned long long i = 0; i < tablelen; i++)
	{
		switch (tablestr[i] & 0xff)
		{
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (std::strtoull(rstr.c_str(), 0, 10) + 1 != std::strtoull(str0.c_str(), 0, 10) || rstr == "")
			{
				if (newtablestr[newloc] == 45)
				{
					newloc++;
					for (unsigned long long i = 0; i < rstr.length(); i++)
					{
						newtablestr[newloc] = rstr[i];
						newloc++;
					}
					newtablestr[newloc] = 44;
					newloc++;
				}
				for (unsigned long long i = 0; i < str0.length(); i++)
				{
					newtablestr[newloc] = str0[i];
					newloc++;
				}
				newtablestr[newloc] = 46;
				newloc++;
			}
			else
			{
				newloc++;
				for (unsigned long long i = 0; i < str0.length(); i++)
				{
					newtablestr[newloc] = str0[i];
					newloc++;
				}
				newtablestr[newloc] = 46;
				newloc++;
			}
			if (step)
			{
				newloc--;
				newtablestr[newloc] = 59;
				newloc++;
				for (unsigned long long i = 0; i < str1.length(); i++)
				{
					newtablestr[newloc] = str1[i];
					newloc++;
				}
				newtablestr[newloc] = 59;
				newloc++;
				for (unsigned long long i = 0; i < str2.length(); i++)
				{
					newtablestr[newloc] = str2[i];
					newloc++;
				}
				newtablestr[newloc] = 46;
				newloc++;
			}
			rstr = "";
			step = 0;
			break;
		case 44: //,
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (std::strtoull(rstr.c_str(), 0, 10) + 1 != std::strtoull(str0.c_str(), 0, 10) || rstr == "")
			{
				if (newtablestr[newloc] == 45)
				{
					newloc++;
					for (unsigned long long i = 0; i < rstr.length(); i++)
					{
						newtablestr[newloc] = rstr[i];
						newloc++;
					}
					newtablestr[newloc] = 44;
					newloc++;
				}
				for (unsigned long long i = 0; i < str0.length(); i++)
				{
					newtablestr[newloc] = str0[i];
					newloc++;
				}
				newtablestr[newloc] = 44;
				newloc++;
			}
			else
			{
				if (newtablestr[newloc] != 45)
				{
					newloc--;
					newtablestr[newloc] = 45;
				}
			}
			rstr = str0;
			step = 0;
			break;
		default: //0-9
			if (cloc > clen - 2)
			{
				clen += 256;
				alc = (char*)realloc(cblock, clen);
				if (!alc)
				{
					free(cblock);
					return 1;
				}
				cblock = alc;
				alc = NULL;
			}
			cblock[cloc] = tablestr[i];
			cloc++;
			break;
		}
	}
	free(cblock);
	char* alc1 = (char*)realloc(tablestr, strlen(newtablestr));
	if (!alc1)
	{
		return 1;
	}
	tablestr = alc1;
	memcpy(tablestr, newtablestr, strlen(newtablestr));
	free(newtablestr);
	cleantablestr(charmap, tablestr);
	return 0;
}

int simptable(HANDLE hDisk, unsigned long sectorsize, char* charmap, unsigned long& tablesize, unsigned long long& extratablesize, unsigned long long filenamecount, char*& fileinfo, char*& filenames, char*& tablestr, char*& table, std::map<unsigned, unsigned> emap, std::map<unsigned, unsigned> dmap)
{
	_LARGE_INTEGER seek = { 0 };
	SetFilePointerEx(hDisk, seek, NULL, 0);
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++)
	{
		if ((tablestr[i] & 0xff) == 46)
		{
			tablelen = i + 1;
		}
	}
	encode(emap, tablestr, tablelen);
	tablelen /= 2;
	unsigned long long filenamesizes = 0;
	for (unsigned long long i = 0; i < strlen(filenames); i++)
	{
		if ((filenames[i] & 0xff) == 255)
		{
			filenamesizes = i + 1;
		}
		if ((filenames[i] & 0xff) == 254)
		{
			break;
		}
	}
	tablesize = (tablelen + filenamesizes + (35 * filenamecount) + sectorsize - 1) / sectorsize + 1;
	settablesize(sectorsize, tablesize, extratablesize, table);
	for (unsigned long long i = 0; i < tablelen; i++)
	{
		table[i + 5] = (unsigned)tablestr[i] & 0xff;
	}
	table[tablelen + 5] = 255;
	for (unsigned long long i = 0; i < filenamesizes; i++)
	{
		table[tablelen + i + 6] = filenames[i];
	}
	table[tablelen + 6 + filenamesizes] = 254;
	for (unsigned long long i = 0; i < filenamecount; i++)
	{
		for (unsigned j = 0; j < 35; j++)
		{
			table[tablelen + filenamesizes + 7 + (i * 35) + j] = fileinfo[(i * 35) + j];
		}
	}
	decode(dmap, tablestr, tablelen);
	cleantablestr(charmap, tablestr);
	DWORD w;
	WriteFile(hDisk, table, ((tablelen + filenamesizes + 7 + (filenamecount * 35) + 511) / 512) * 512, &w, NULL);
	if (w != ((tablelen + filenamesizes + 7 + (filenamecount * 35) + 511) / 512) * 512)
	{
		return 1;
	}
	return 0;
}

int createfile(PWSTR filename, unsigned long gid, unsigned long uid, unsigned long mode, unsigned long winattrs, unsigned long long& filenamecount, char*& fileinfo, char*& filenames, char* charmap, char*& tablestr)
{
	char* alc = (char*)realloc(fileinfo, (filenamecount + 1) * 35);
	if (!alc)
	{
		return 1;
	}
	fileinfo = alc;
	char* file = (char*)calloc(wcslen(filename) + 1, 1);
	if (!file)
	{
		return 1;
	}
	for (unsigned i = 0; i < wcslen(filename); i++)
	{
		file[i] = filename[i] & 0xff;
	}
	unsigned long long oldlen = 0;
	for (unsigned long long i = 0; i < strlen(filenames); i++)
	{
		if ((filenames[i] & 0xff) == 255)
		{
			oldlen = i + 1;
		}
	}
	strcpy_s(filenames + oldlen, strlen(file) + 1, file);
	filenames[oldlen + strlen(file)] = 255;
	filenames[oldlen + strlen(file) + 1] = 254;
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++)
	{
		if ((tablestr[i] & 0xff) == 46)
		{
			tablelen = i + 1;
		}
	}
	tablestr[tablelen] = 46;
	char* gum = (char*)calloc((filenamecount + 1) * 11, 1);
	if (!gum)
	{
		free(file);
		return 1;
	}
	memcpy(gum, fileinfo + filenamecount * 24, filenamecount * 11);
	time_t ltime;
	time(&ltime);
	double t = (double)ltime;
	char ti[8] = { 0 };
	memcpy(ti, &t, 8);
	char tim[8] = { 0 };
	for (unsigned i = 0; i < 8; i++)
	{
		tim[i] = ti[7 - i];
	}
	memcpy(fileinfo + filenamecount * 24, &tim, 8);
	memcpy(fileinfo + filenamecount * 24 + 8, &tim, 8);
	memcpy(fileinfo + filenamecount * 24 + 16, &tim, 8);
	char guidmodes[11] = { 0 };
	guidmodes[0] = (gid >> 16) & 0xff;
	guidmodes[1] = (gid >> 8) & 0xff;
	guidmodes[2] = gid & 0xff;
	guidmodes[3] = (uid >> 8) & 0xff;
	guidmodes[4] = uid & 0xff;
	guidmodes[5] = (mode >> 8) & 0xff;
	guidmodes[6] = mode & 0xff;
	winattrs |= 2048;
	guidmodes[7] = (winattrs >> 24) & 0xff;
	guidmodes[8] = (winattrs >> 16) & 0xff;
	guidmodes[9] = (winattrs >> 8) & 0xff;
	guidmodes[10] = winattrs & 0xff;
	for (unsigned i = 0; i < 11; i++)
	{
		gum[filenamecount * 11 + i] = guidmodes[i];
	}
	for (unsigned long long i = 0; i < (filenamecount + 1) * 11; i++)
	{
		fileinfo[(filenamecount + 1) * 24 + i] = gum[i];
	}
	free(gum);
	filenamecount++;
	cleantablestr(charmap, tablestr);
	return 0;
}

int deletefile(unsigned long long index, unsigned long long filenameindex, unsigned long long filenamestrindex, unsigned long long& filenamecount, char*& fileinfo, char*& filenames, char*& tablestr)
{
	unsigned start = 0;
	unsigned filenamelen = 0;
	unsigned long long end = 0;
	for (unsigned long long i = 0; i < filenamestrindex; i++)
	{
		start = filenames[filenamestrindex - i - 1] & 0xff;
		if (start == 255 || start == 42)
		{
			break;
		}
		filenamelen++;
	}
	if (start != 42)
	{
		for (; end < strlen(filenames); end++)
		{
			if ((filenames[filenamestrindex + end] & 0xff) == 255)
			{
				break;
			}
		}
		unsigned long long pindex = getpindex(index, tablestr);
		memcpy(tablestr + index - pindex, tablestr + index + 1, strlen(tablestr) - index - 1);
		tablestr[strlen(tablestr) - pindex - 1] = 0;
		memcpy(fileinfo + filenameindex * 24, fileinfo + (filenameindex + 1) * 24, (filenamecount - filenameindex - 1) * 24);
		memcpy(fileinfo + (filenamecount - 1) * 24 + filenameindex * 11, fileinfo + filenamecount * 24 + (filenameindex + 1) * 11, (filenamecount - filenameindex - 1) * 11);
		fileinfo[(filenamecount - 1) * 35] = 0;
	}
	memcpy(filenames + filenamestrindex - filenamelen - 1, filenames + filenamestrindex + end, strlen(filenames) - filenamestrindex - end + 1);
	filenamecount--;
	return 0;
}

int renamefile(PWSTR oldfilename, PWSTR newfilename, unsigned long long& filenamestrindex, char*& filenames)
{
	unsigned long long oldlen = strlen(filenames);
	char* coldfilename = (char*)calloc(wcslen(oldfilename) + 1, 1);
	char* cnewfilename = (char*)calloc(wcslen(newfilename) + 1, 1);
	char* files = (char*)calloc(oldlen - filenamestrindex + 1, 1);
	for (unsigned long long i = 0; i < wcslen(oldfilename); i++)
	{
		coldfilename[i] = oldfilename[i] & 0xff;
	}
	for (unsigned long long i = 0; i < wcslen(newfilename); i++)
	{
		cnewfilename[i] = newfilename[i] & 0xff;
	}
	memcpy(files, filenames + filenamestrindex, oldlen - filenamestrindex);
	memcpy(filenames + filenamestrindex - strlen(coldfilename), cnewfilename, strlen(cnewfilename));
	unsigned long long afterlen = 0;
	for (; afterlen < strlen(files); afterlen++)
	{
		if ((files[afterlen] & 0xff) == 254)
		{
			break;
		}
	}
	memcpy(filenames + filenamestrindex - strlen(coldfilename) + strlen(cnewfilename), files, afterlen + 1);
	filenamestrindex -= strlen(coldfilename) - strlen(cnewfilename);
	free(coldfilename);
	free(cnewfilename);
	free(files);
	return 0;
}

void readwrite(HANDLE hDisk, unsigned long sectorsize, unsigned long long disksize, unsigned long long start, unsigned step, unsigned range, unsigned long long len, std::string str0, std::string str1, std::string str2, std::string rstr, unsigned long long& rblock, unsigned long long& block, char*& buf, unsigned rw)
{
	LARGE_INTEGER loc{};
	DWORD wr;
	if (range)
	{
		for (unsigned long long p = std::strtoull(rstr.c_str(), 0, 10); p < std::strtoull(str0.c_str(), 0, 10) + 1; p++)
		{
			if (start / sectorsize <= block && block < (start + len + sectorsize - 1) / sectorsize)
			{
				if (start / sectorsize == block)
				{ // Start block
					if (step)
					{
						loc.QuadPart = disksize - (p * sectorsize + sectorsize) + start % sectorsize + std::strtoul(str1.c_str(), 0, 10);
						SetFilePointerEx(hDisk, loc, NULL, 0);
						if (len - start % sectorsize < std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10))
						{
							if (!rw)
							{
								if (!ReadFile(hDisk, buf, len, &wr, NULL)) return;
							}
							else
							{
								if (!WriteFile(hDisk, buf, len, &wr, NULL)) return;
							}
							rblock += len;
						}
						else
						{
							if (!rw)
							{
								if (!ReadFile(hDisk, buf, std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL)) return;
							}
							else
							{
								if (!WriteFile(hDisk, buf, std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL)) return;
							}
							rblock += std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize;
						}
					}
					else
					{
						loc.QuadPart = disksize - (p * sectorsize + sectorsize) + start % sectorsize;
						SetFilePointerEx(hDisk, loc, NULL, 0);
						if (!rw)
						{
							if (!ReadFile(hDisk, buf, min(sectorsize - start % sectorsize, len), &wr, NULL)) return;
						}
						else
						{
							if (!WriteFile(hDisk, buf, min(sectorsize - start % sectorsize, len), &wr, NULL)) return;
						}
						rblock += min(sectorsize - start % sectorsize, len);
					}
				}
				else if (len - rblock <= sectorsize)
				{ // End block
					if (step)
					{
						loc.QuadPart = disksize - (p * sectorsize + sectorsize) + std::strtoul(str1.c_str(), 0, 10);
					}
					else
					{
						loc.QuadPart = disksize - (p * sectorsize + sectorsize);
					}
					SetFilePointerEx(hDisk, loc, NULL, 0);
					if (!rw)
					{
						if (!ReadFile(hDisk, buf + rblock, len - rblock, &wr, NULL)) return;
					}
					else
					{
						if (!WriteFile(hDisk, buf + rblock, len - rblock, &wr, NULL)) return;
					}
					rblock = len;
				}
				else
				{ // In between blocks
					loc.QuadPart = disksize - (p * sectorsize + sectorsize);
					SetFilePointerEx(hDisk, loc, NULL, 0);
					if (!rw)
					{
						if (!ReadFile(hDisk, buf + rblock, sectorsize, &wr, NULL)) return;
					}
					else
					{
						if (!WriteFile(hDisk, buf + rblock, sectorsize, &wr, NULL)) return;
					}
					rblock += sectorsize;
				}
			}
			block++;
		}
		block--;
		return;
	}
	if (start / sectorsize <= block && block < (start + len + sectorsize - 1) / sectorsize)
	{
		if (start / sectorsize == block)
		{ // Start block
			if (step)
			{
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize) + start % sectorsize + std::strtoul(str1.c_str(), 0, 10);
				SetFilePointerEx(hDisk, loc, NULL, 0);
				if (len - start % sectorsize < std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10))
				{
					if (!rw)
					{
						if (!ReadFile(hDisk, buf, len, &wr, NULL)) return;
					}
					else
					{
						if (!WriteFile(hDisk, buf, len, &wr, NULL)) return;
					}
					rblock += len;
				}
				else
				{
					if (!rw)
					{
						if (!ReadFile(hDisk, buf, std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL)) return;
					}
					else
					{
						if (!WriteFile(hDisk, buf, std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL)) return;
					}
					rblock += std::strtoull(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize;
				}
			}
			else
			{
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize) + start % sectorsize;
				SetFilePointerEx(hDisk, loc, NULL, 0);
				if (!rw)
				{
					if (!ReadFile(hDisk, buf, min(sectorsize - start % sectorsize, len), &wr, NULL)) return;
				}
				else
				{
					if (!WriteFile(hDisk, buf, min(sectorsize - start % sectorsize, len), &wr, NULL)) return;
				}
				rblock += min(sectorsize - start % sectorsize, len);
			}
		}
		else if (len - rblock <= sectorsize)
		{ // End block
			if (step)
			{
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize) + std::strtoul(str1.c_str(), 0, 10);
			}
			else
			{
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize);
			}
			SetFilePointerEx(hDisk, loc, NULL, 0);
			if (!rw)
			{
				if (!ReadFile(hDisk, buf + rblock, len - rblock, &wr, NULL)) return;
			}
			else
			{
				if (!WriteFile(hDisk, buf + rblock, len - rblock, &wr, NULL)) return;
			}
			rblock = len;
		}
		else
		{ // In between blocks
			loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize);
			SetFilePointerEx(hDisk, loc, NULL, 0);
			if (!rw)
			{
				if (!ReadFile(hDisk, buf + rblock, sectorsize, &wr, NULL)) return;
			}
			else
			{
				if (!WriteFile(hDisk, buf + rblock, sectorsize, &wr, NULL)) return;
			}
			rblock += sectorsize;
		}
	}
}

void chtime(char*& fileinfo, unsigned long long filenameindex, double& time, unsigned ch)
{ // 24 bytes per file
	unsigned o = 0;
	if (ch == 2 || ch == 3)
	{
		o = 8;
	}
	else if (ch == 4 || ch == 5)
	{
		o = 16;
	}
	if (!(ch % 2))
	{
		char tim[8] = { 0 };
		memcpy(tim, fileinfo + filenameindex * 24 + o, 8);
		char ti[8] = { 0 };
		for (unsigned i = 0; i < 8; i++)
		{
			ti[i] = tim[7 - i];
		}
		memcpy(&time, ti, 8);
	}
	else
	{
		char ti[8] = { 0 };
		memcpy(ti, &time, 8);
		char tim[8] = { 0 };
		for (unsigned i = 0; i < 8; i++)
		{
			tim[i] = ti[7 - i];
		}
		memcpy(fileinfo + filenameindex * 24 + o, tim, 8);
	}
}

void chgid(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& gid, unsigned ch)
{ // Next three bytes of fileinfo after times
	if (!ch)
	{
		gid = (fileinfo[filenamecount * 24 + filenameindex * 11] & 0xff) << 16 | (fileinfo[filenamecount * 24 + filenameindex * 11 + 1] & 0xff) << 8 | fileinfo[filenamecount * 24 + filenameindex * 11 + 2] & 0xff;
	}
	else
	{
		fileinfo[filenamecount * 24 + filenameindex * 11] = (gid >> 16) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 1] = (gid >> 8) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 2] = gid & 0xff;
	}
}

void chuid(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& uid, unsigned ch)
{ // Next two bytes of fileinfo
	if (!ch)
	{
		uid = (fileinfo[filenamecount * 24 + filenameindex * 11 + 3] & 0xff) << 8 | fileinfo[filenamecount * 24 + filenameindex * 11 + 4] & 0xff;
	}
	else
	{
		fileinfo[filenamecount * 24 + filenameindex * 11 + 3] = (uid >> 8) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 4] = uid & 0xff;
	}
}

void chmode(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& mode, unsigned ch)
{ // Next two bytes of fileinfo
	if (!ch)
	{
		mode = (fileinfo[filenamecount * 24 + filenameindex * 11 + 5] & 0xff) << 8 | fileinfo[filenamecount * 24 + filenameindex * 11 + 6] & 0xff;
	}
	else
	{
		fileinfo[filenamecount * 24 + filenameindex * 11 + 5] = (mode >> 8) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 6] = mode & 0xff;
	}
}

void chwinattrs(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& winattrs, unsigned ch)
{ // Last four bytes of fileinfo
	if (!ch)
	{
		winattrs = (fileinfo[filenamecount * 24 + filenameindex * 11 + 7] & 0xff) << 24 | (fileinfo[filenamecount * 24 + filenameindex * 11 + 8] & 0xff) << 16 | (fileinfo[filenamecount * 24 + filenameindex * 11 + 9] & 0xff) << 8 | fileinfo[filenamecount * 24 + filenameindex * 11 + 10] & 0xff;
	}
	else
	{
		fileinfo[filenamecount * 24 + filenameindex * 11 + 7] = (winattrs >> 24) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 8] = (winattrs >> 16) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 9] = (winattrs >> 8) & 0xff;
		fileinfo[filenamecount * 24 + filenameindex * 11 + 10] = winattrs & 0xff;
	}
}

int readwritefile(HANDLE hDisk, unsigned long long sectorsize, unsigned long long index, unsigned long long start, unsigned long long len, unsigned long long disksize, char* tablestr, char*& buf, char*& fileinfo, unsigned long long filenameindex, unsigned rw)
{
	unsigned long long pindex = getpindex(index, tablestr);
	unsigned long long filesize = 0;
	getfilesize(sectorsize, index, tablestr, filesize);
	len = min(len, filesize - start);
	index++;
	pindex++;
	unsigned long long block = 0;
	unsigned long long rblock = 0;
	char* alc = NULL;
	unsigned long long clen = 256;
	char* cblock = (char*)calloc(clen, 1);
	if (!cblock)
	{
		return 1;
	}
	unsigned long long cloc = 0;
	std::string str0;
	std::string str1;
	std::string str2;
	std::string rstr;
	unsigned step = 0;
	unsigned range = 0;
	for (unsigned long long i = 0; i < pindex; i++)
	{
		switch (tablestr[index - pindex + i] & 0xff)
		{
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			readwrite(hDisk, sectorsize, disksize, start, step, range, len, str0, str1, str2, rstr, rblock, block, buf, rw);
			step = 0;
			range = 0;
			break;
		case 45: //-
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step = 0;
			range++;
			rstr = str0;
			break;
		case 44: //,
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			readwrite(hDisk, sectorsize, disksize, start, step, range, len, str0, str1, str2, rstr, rblock, block, buf, rw);
			step = 0;
			range = 0;
			block++;
			break;
		default: //0-9
			if (cloc > clen - 2)
			{
				clen += 256;
				alc = (char*)realloc(cblock, clen);
				if (!alc)
				{
					free(cblock);
					return 1;
				}
				cblock = alc;
				alc = NULL;
			}
			cblock[cloc] = tablestr[index - pindex + i];
			cloc++;
			break;
		}
	}
	free(cblock);
	time_t ltime;
	time(&ltime);
	double ctime = (double)ltime;
	chtime(fileinfo, filenameindex, ctime, rw * 2 + 1);
	return 0;
}

int trunfile(HANDLE hDisk, unsigned long sectorsize, unsigned long long& index, unsigned long tablesize, unsigned long long disksize, unsigned long long size, unsigned long long newsize, unsigned long long filenameindex, char* charmap, char*& tablestr, char*& fileinfo, unsigned long long& usedblocks)
{
	desimp(charmap, tablestr);
	if (size < newsize)
	{
		if (size % sectorsize)
		{
			char* temp = (char*)calloc(size % sectorsize, 1);
			readwritefile(hDisk, sectorsize, index, size - size % sectorsize, size % sectorsize, disksize, tablestr, temp, fileinfo, filenameindex, 0);
			dealloc(sectorsize, charmap, tablestr, index, size, size % sectorsize);
			alloc(sectorsize, disksize, tablesize, charmap, tablestr, index, newsize - (size - size % sectorsize), usedblocks);
			readwritefile(hDisk, sectorsize, index, size - size % sectorsize, size % sectorsize, disksize, tablestr, temp, fileinfo, filenameindex, 1);
			free(temp);
			size += newsize - size;
		}
		alloc(sectorsize, disksize, tablesize, charmap, tablestr, index, newsize - size, usedblocks);
	}
	if (size > newsize)
	{
		if (size % sectorsize && size - newsize > size % sectorsize)
		{
			dealloc(sectorsize, charmap, tablestr, index, size, size % sectorsize);
			size -= size % sectorsize;
		}
		dealloc(sectorsize, charmap, tablestr, index, size, size - newsize);
	}
	simp(charmap, tablestr);
	time_t ltime;
	time(&ltime);
	double ctime = (double)ltime;
	chtime(fileinfo, filenameindex, ctime, 3);
	return 0;
}

/*int main(int argc, char* argv[])
{
	if (argc == 1)
	{
		std::cout << "Usage: SpaceFS <disk> <format sectorsize>" << std::endl;
		return 1;
	}

	_tzset();

	wchar_t cDisk[MAX_PATH];
	mbstowcs_s(NULL, cDisk, argv[1], MAX_PATH);
	LPCWSTR lDisk = cDisk;
	unsigned long sectorsize = 512;

	//std::cout << "Opening disk: " << argv[1] << std::endl;
	HANDLE hDisk = CreateFile(lDisk,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (hDisk == INVALID_HANDLE_VALUE)
	{
		std::cout << "Opening Error: " << GetLastError() << std::endl;
		return 1;
	}
	//std::cout << "Disk opened successfully" << std::endl;

	if (argc == 3)
	{
		sscanf_s(argv[2], "%d", &sectorsize);
		unsigned i = 0;
		while (sectorsize != 1)
		{
			sectorsize >>= 1;
			i++;
		}
		if (i < 9)
		{
			i = 9;
		}
		i -= 9;
		char bytes[512] = { 0 };
		bytes[0] = i;
		bytes[5] = 255;
		bytes[6] = 254;
		DWORD w;
		WriteFile(hDisk, bytes, 512, &w, NULL);
		if (w != 512)
		{
			std::cout << "Formatting Error: " << GetLastError() << std::endl;
			return 1;
		}
		//std::cout << "Formatted disk with sectorsize: " << unsigned(pow(2, 9 + i)) << ", 2**(9+" << i << ")" << std::endl;
	}

	_LARGE_INTEGER disksize = { 0 };
	SetFilePointerEx(hDisk, disksize, &disksize, 2);
	//std::cout << "Disk size: " << disksize.QuadPart << std::endl;

	_LARGE_INTEGER seek = { 0 };
	SetFilePointerEx(hDisk, seek, &seek, 0);
	char bytes[512] = { 0 };
	DWORD r;
	if (!ReadFile(hDisk, bytes, 512, &r, NULL))
	{
		std::cout << "Reading Error: " << GetLastError() << std::endl;
		return 1;
	}
	if (r != 512)
	{
		std::cout << "Reading Error: " << GetLastError() << std::endl;
		return 1;
	}
	sectorsize = pow(2, 9 + bytes[0]);
	//std::cout << "Read disk with sectorsize: " << sectorsize << std::endl;

	unsigned long tablesize = 1 + bytes[4] + (bytes[3] << 8) + (bytes[2] << 16) + (bytes[1] << 24);
	unsigned long long extratablesize = (static_cast<unsigned long long>(tablesize) * sectorsize) - 512;
	char* ttable = (char*)calloc(extratablesize, 1);
	if (!ReadFile(hDisk, ttable, extratablesize, &r, NULL))
	{
		std::cout << "Reading table Error: " << GetLastError() << std::endl;
		return 1;
	}
	if (r != extratablesize)
	{
		std::cout << "Reading table Error: " << GetLastError() << std::endl;
		return 1;
	}
	//std::cout << "Read table with size: " << tablesize << std::endl;

	char* table = (char*)calloc(512 + static_cast<size_t>(extratablesize), 1);
	memcpy(table, bytes, 512);
	memcpy(table + 512, ttable, extratablesize);
	free(ttable);

	char charmap[16] = "0123456789-,.; ";
	std::map<unsigned, unsigned> emap = {};
	std::map<unsigned, unsigned> dmap = {};
	unsigned p = 0;
	unsigned c;
	for (unsigned i = 0; i < 15; i++)
	{
		for (unsigned o = 0; o < 15; o++)
		{
			c = charmap[i] << 8 | charmap[o];
			emap[c] = p;
			dmap[p] = c;
			p++;
		}
	}

	unsigned long long pos = 0;
	while (((unsigned)table[pos] & 0xff) != 255)
	{
		pos++;
	}
	//std::cout << "Found end of table at: " << pos << std::endl;
	//std::cout << "Table size: " << pos - 5 << std::endl;
	char* tablestr = (char*)calloc(pos - 5, 1);
	memcpy(tablestr, table + 5, pos - 5);
	decode(dmap, tablestr, pos - 5);
	//std::cout << "Decoded table: " << std::string(str, (pos - 5) * 2) << std::endl;

	//simp(charmap, tablestr);
	//encode(emap, str, (pos - 5) * 2);
	//std::cout << "Encoded table: ";
	//for (unsigned long long i = 0; i < pos - 5; i++)
	{
	//	std::cout << ((unsigned)str[i] & 0xff) << " ";
	//}
	//std::cout << std::endl;

	unsigned long long filenamepos = pos;
	while (((unsigned)table[filenamepos] & 0xff) != 254)
	{
		filenamepos++;
	}
	//std::cout << "Found end of filenames at: " << filenamepos << std::endl;
	//std::cout << "Filenames size: " << filenamepos - pos - 1 << std::endl;
	char* filenames = (char*)calloc(filenamepos - pos - 1, 1);
	memcpy(filenames, table + pos + 1, filenamepos - pos - 1);
	unsigned long long filenamecount = 0;
	for (unsigned long long i = 0; i < filenamepos - pos - 1; i++)
	{
		if (((unsigned)filenames[i] & 0xff) == 255)
		{
			filenamecount++;
		}
	}
	filenames[filenamepos - pos - 1] = 254;
	//std::cout << "Filename count: " << filenamecount << std::endl;
	//std::cout << "Filenames: " << std::string(filenames, filenamepos - pos - 1) << std::endl;

	char* fileinfo = (char*)calloc(filenamecount, 35);
	memcpy(fileinfo, table + filenamepos + 1, filenamecount * 35);
	//std::cout << "Fileinfo size: " << filenamecount * 35 << std::endl;
	//std::cout << "Fileinfo: " << std::string(fileinfo, filenamecount * 35) << std::endl;

	unsigned long long usedblocks = 0;

	createfile((PWSTR)L"/Test.bin", 545, 545, 448, 2048, filenamecount, fileinfo, filenames, charmap, tablestr);
	createfile((PWSTR)L"/Test", 545, 1000, 448, 2048, filenamecount, fileinfo, filenames, charmap, tablestr);
	unsigned long long tindex = gettablestrindex((PWSTR)"/Test", filenames, tablestr, filenamecount);
	unsigned long long tfilenameindex = 0;
	unsigned long long tfilenamestrindex = 0;
	getfilenameindex((PWSTR)L"/Test", filenames, filenamecount, tfilenameindex, tfilenamestrindex);
	trunfile(hDisk, sectorsize, tindex, tablesize, disksize.QuadPart, 0, 10, tfilenameindex, charmap, tablestr, fileinfo, usedblocks);
	unsigned long long index = gettablestrindex((PWSTR)L"/Test.bin", filenames, tablestr, filenamecount);
	unsigned long long filenameindex = 0;
	unsigned long long filenamestrindex = 0;
	getfilenameindex((PWSTR)L"/Test.bin", filenames, filenamecount, filenameindex, filenamestrindex);
	alloc(sectorsize, disksize.QuadPart, tablesize, charmap, tablestr, index, 2097152 * 3 + 512, usedblocks);
	unsigned long long filesize = 0;
	getfilesize(sectorsize, index, tablestr, filesize);
	std::cout << filesize << " " << tablestr << " " << usedblocks << std::endl;
	dealloc(sectorsize, charmap, tablestr, index, filesize, 2097152 * 2 + 512);
	getfilesize(sectorsize, index, tablestr, filesize);
	std::cout << filesize << " " << tablestr << " " << usedblocks << std::endl;
	alloc(sectorsize, disksize.QuadPart, tablesize, charmap, tablestr, index, 512, usedblocks);
	getfilesize(sectorsize, index, tablestr, filesize);
	std::cout << filesize << " " << tablestr << " " << usedblocks << std::endl;
	simptable(hDisk, sectorsize, charmap, tablesize, extratablesize, filenamecount, fileinfo, filenames, tablestr, table, emap, dmap);
	char* buf = (char*)calloc(2097152 * 3, 1);
	readwritefile(hDisk, sectorsize, index, 0, 2097152 * 3, disksize.QuadPart, tablestr, buf, fileinfo, filenameindex, 1);
	std::cout << tablestr << std::endl;
	trunfile(hDisk, sectorsize, index, tablesize, disksize.QuadPart, filesize, 2097152 * 2, filenameindex, charmap, tablestr, fileinfo, usedblocks);
	std::cout << tablestr << " " << usedblocks << std::endl;
	simptable(hDisk, sectorsize, charmap, tablesize, extratablesize, filenamecount, fileinfo, filenames, tablestr, table, emap, dmap);
	std::cout << tablestr << std::endl;
	double time = 0;
	unsigned long gid = 0;
	unsigned long uid = 0;
	unsigned long mode = 16877;
	unsigned long winattrs = 0;
	chtime(fileinfo, filenameindex, time, 1);
	chtime(fileinfo, filenameindex, time, 3);
	chtime(fileinfo, filenameindex, time, 5);
	chgid(fileinfo, filenamecount, filenameindex, gid, 0);
	chuid(fileinfo, filenamecount, filenameindex, uid, 0);
	chmode(fileinfo, filenamecount, filenameindex, mode, 1);
	chwinattrs(fileinfo, filenamecount, filenameindex, winattrs, 0);
	std::cout << (time_t)time << std::endl;
	std::cout << gid << " " << uid << " " << mode << " " << winattrs << std::endl;
	renamefile((PWSTR)L"/Test.bin", (PWSTR)L"/Testing.txt", filenamestrindex, filenames);
	std::cout << filenames << std::endl;
	deletefile(index, filenameindex, filenamestrindex, filenamecount, fileinfo, filenames, tablestr);
	chtime(fileinfo, 0, time, 4);
	chuid(fileinfo, 1, 0, uid, 0);
	std::cout << uid << " " << (time_t)time << std::endl;
	std::cout << fileinfo << " " << filenames << " " << tablestr << std::endl;
	tindex = gettablestrindex((PWSTR)L"/Test", filenames, tablestr, filenamecount);
	tfilenameindex = 0;
	tfilenamestrindex = 0;
	getfilenameindex((PWSTR)L"/Test", filenames, filenamecount, tfilenameindex, tfilenamestrindex);
	deletefile(tindex, tfilenameindex, tfilenamestrindex, filenamecount, fileinfo, filenames, tablestr);
	simptable(hDisk, sectorsize, charmap, tablesize, extratablesize, filenamecount, fileinfo, filenames, tablestr, table, emap, dmap);
	std::cout << tablestr << std::endl;
	return 0;
}*/