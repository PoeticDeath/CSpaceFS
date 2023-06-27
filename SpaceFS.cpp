#include <windows.h>
#include <stdio.h>
#include <iostream>
#include <map>
#include <time.h>
#include <string>

void encode(std::map<unsigned, unsigned> emap, char*& str, unsigned long long& len) {
	if (len % 2 != 0) {
		len++;
		char* alc = (char*)realloc(str, len);
		if (alc == NULL) {
			return;
		}
		str = alc;
		str[len - 1] = 32;
		str[len - 2] = 46;
	}
	char* bytes = (char*)calloc(len / 2, 1);
	for (unsigned long long i = 0; i < len; i += 2) {
		bytes[i / 2] = emap[str[i] << 8 | str[i + 1]];
	}
	free(str);
	str = bytes;
}

void decode(std::map<unsigned, unsigned> dmap, char*& bytes, unsigned long long len) {
	char* str = (char*)calloc(len * 2, 1);
	unsigned d;
	for (unsigned long long i = 0; i < len; i++) {
		d = dmap[bytes[i] & 0xff];
		str[i * 2] = d >> 8;
		str[i * 2 + 1] = d & 0xff;
	}
	free(bytes);
	bytes = str;
}

void cleantablestr(char* charmap, char*& tablestr) {
	unsigned long long i = 0;
	unsigned b = 0;
	for (; i < strlen(tablestr); i++) {
		b = 1;
		for (unsigned o = 0; o < strlen(charmap); o++) {
			if (tablestr[i] == charmap[o]) {
				b = 0;
				break;
			}
		}
		if (b == 1) {
			break;
		}
	}
	char* alc = (char*)calloc(i + 1, 1);
	for (unsigned long long o = 0; o < i; o++) {
		alc[o] = tablestr[o];
	}
	tablestr = alc;
}

int settablesize(unsigned long sectorsize, unsigned long& tablesize, unsigned long& extratablesize, char*& table) {
	extratablesize = (tablesize * sectorsize);
	char* alc = NULL;
	alc = (char*)realloc(table, extratablesize);
	if (alc == NULL) {
		tablesize = 1 + table[4] + (table[3] << 8) + (table[2] << 16) + (table[1] << 24);
		extratablesize = (tablesize * sectorsize);
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

void resetcloc(unsigned long long& cloc, char*& cblock, unsigned long long clen, std::string& str0, std::string& str1, std::string& str2, unsigned step) {
	std::string str = std::string(cblock + strlen(cblock) - cloc, cloc);
	switch (step) {
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
	cblock = (char*)calloc(clen, 1);
}

unsigned long long getpindex(unsigned long long index, char* tablestr) {
	unsigned long long pindex = 0;
	while ((tablestr[index - pindex - 1] & 0xff) != 46) {
		pindex++;
		if (index - pindex == 0) {
			break;
		}
	}
	return pindex;
}

int getfilesize(unsigned long sectorsize, unsigned long long index, char* tablestr, unsigned long long& filesize) {
	unsigned long long pindex = getpindex(index, tablestr);
	if (index == 0 || pindex == 0) {
		filesize = 0;
		return 0;
	}
	unsigned long long o = 0;
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
	for (unsigned long long i = 0; i < pindex + 1; i++) {
		switch (tablestr[index - pindex + i] & 0xff) {
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			if (step != 0) {
				o += std::stoull(str2, 0, 10) - std::stoull(str1, 0, 10);
			}
			else if (range == 0) {
				o += sectorsize;
			}
			if (range != 0) {
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
			if (step != 0) {
				o += std::stoull(str2, 0, 10) - std::stoull(str1, 0, 10);
			}
			else if (range == 0) {
				o += sectorsize;
			}
			if (range != 0) {
				o += (sectorsize * (std::stoull(str0, 0, 10) - std::stoull(rstr, 0, 10) + 1));
			}
			step = 0;
			range = 0;
			break;
		default: //0-9
			if (cloc > clen - 2) {
				clen += 256;
				alc = (char*)realloc(cblock, clen);
				if (alc == NULL) {
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
	filesize = o;
	return 0;
}

void addtopartlist(unsigned long sectorsize, unsigned range, unsigned step, std::string str0, std::string str1, std::string str2, std::string rstr, std::map<std::string, std::map<unsigned long, unsigned long long>>& partlist, std::map<std::string, unsigned long>& list) {
	if (str0 == "") {
		return;
	}
	if (range == 0) {
		if (step == 0) {
			list[str0] = 0;
			//std::cout << str0 << std::endl;
		}
	}
	else {
		for (unsigned long long i = std::strtoull(rstr.c_str(), 0, 10); i < std::strtoull(str0.c_str(), 0, 10) + 1; i++) {
			list[std::to_string(i)] = 0;
		}
		//std::cout << rstr << "-" << str0 << ",";
	}
	if (step != 0) {
		for (unsigned long i = std::strtoul(str1.c_str(), 0, 10); i < std::strtoul(str2.c_str(), 0, 10); i++) {
			if (i / 64 < std::strtoul(str2.c_str(), 0, 10) / 64) {
				partlist[str0][i / 64] |= 0xffffffffffffffff << i % 64;
				list[str0] -= 64 - i % 64;
				i += 63 - i % 64;
			}
			else {
				partlist[str0][i / 64] |= static_cast<unsigned long long>(1) << i % 64;
				list[str0]--;
			}
		}
		//std::cout << str0 << ";" << str1 << ";" << str2 << std::endl;
	}
}

int findblock(unsigned long sectorsize, unsigned long long disksize, unsigned long tablesize, char* tablestr, char*& block, unsigned long long& blockstrlen, unsigned long blocksize) {
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++) {
		if ((tablestr[i] & 0xff) == 46) {
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
	for (unsigned long i = 0; i < disksize / sectorsize - tablesize; i++) {
		list[std::to_string(i)] = sectorsize;
	}
	for (unsigned long long i = 0; i < tablelen; i++) {
		switch (tablestr[i] & 0xff) {
		case 59: //;
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			step++;
			break;
		case 46: //.
			resetcloc(cloc, cblock, clen, str0, str1, str2, step);
			addtopartlist(sectorsize, range, step, str0, str1, str2, rstr, partlist, list);
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
			addtopartlist(sectorsize, range, step, str0, str1, str2, rstr, partlist, list);
			step = 0;
			range = 0;
			break;
		default: //0-9
			if (cloc > clen - 2) {
				clen += 256;
				alc = (char*)realloc(cblock, clen);
				if (alc == NULL) {
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
	unsigned long bytecount = 0;
	unsigned long o = 0;
	std::string s;
	for (unsigned long long i = 0; i < disksize / sectorsize - tablesize; i++) {
		if (list[std::to_string(i)] >= blocksize) {
			o = 0;
			if (list[std::to_string(i)] == sectorsize) {
				bytecount = blocksize;
			}
			while (bytecount < blocksize && o < sectorsize) {
				if ((partlist[std::to_string(i)][o / 64] & static_cast<unsigned long long>(1) << o % 64) == 0) {
					bytecount++;
				}
				else {
					o++;
					bytecount = 0;
				}
				if (blocksize > sectorsize - o) {
					break;
				}
			}
			if (bytecount == blocksize) {
				if (blocksize % sectorsize != 0) {
					s = std::to_string(i) + ";" + std::to_string(o) + ";" + std::to_string(o + bytecount);
				}
				else {
					s = std::to_string(i);
				}
				break;
			}
		}
	}
	if (s == "") {
		return 1;
	}
	block = (char*)calloc(strlen(s.c_str()), 1);
	unsigned long long i = 0;
	for (; i < strlen(s.c_str()); i++) {
		block[i] = s[i];
	}
	blockstrlen = i;
	return 0;
}

int alloc(unsigned long sectorsize, unsigned long long disksize, unsigned long tablesize, char* charmap, char*& tablestr, unsigned long long& index, unsigned long long size) {
	char* block = NULL;
	unsigned long long blockstrlen = 0;
	unsigned long long o = 0;
	if ((tablestr[index - 1] & 0xff) != 46 && index != 0) {
		o++;
	}
	for (unsigned long long p = 0; p < size / sectorsize; p++) {
		findblock(sectorsize, disksize, tablesize, tablestr, block, blockstrlen, sectorsize);
		if (block == NULL) {
			return 1;
		}
		char* alc1 = (char*)calloc(strlen(tablestr) - index, 1);
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++) {
			alc1[i] = tablestr[index + i];
		}
		char* alc2 = (char*)realloc(tablestr, strlen(tablestr) + blockstrlen + 1);
		if (o != 0) {
			alc2[index] = 44;
		}
		for (unsigned long long i = 0; i < blockstrlen; i++) {
			alc2[index + o + i] = block[i];
		}
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++) {
			alc2[index + o + blockstrlen + i] = alc1[i];
		}
		tablestr = alc2;
		index += blockstrlen;
		if (o != 0) {
			index++;
		}
		o = 1;
		cleantablestr(charmap, tablestr);
	}
	if (size % sectorsize != 0) {
		findblock(sectorsize, disksize, tablesize, tablestr, block, blockstrlen, size % sectorsize);
		if (block == NULL) {
			return 1;
		}
		char* alc1 = (char*)calloc(strlen(tablestr) - index, 1);
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++) {
			alc1[i] = tablestr[index + i];
		}
		char* alc2 = (char*)realloc(tablestr, strlen(tablestr) + blockstrlen + 1);
		if (o != 0) {
			alc2[index] = 44;
		}
		for (unsigned long long i = 0; i < blockstrlen; i++) {
			alc2[index + o + i] = block[i];
		}
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++) {
			alc2[index + blockstrlen + o + i] = alc1[i];
		}
		tablestr = alc2;
		index += blockstrlen;
		if (o != 0) {
			index++;
		}
		cleantablestr(charmap, tablestr);
	}
	return 0;
}

int dealloc(unsigned long sectorsize, char* charmap, char*& tablestr, unsigned long long& index, unsigned long long filesize, unsigned long long size) {
	if (size % sectorsize != 0) {
		unsigned long long pindex = getpindex(index, tablestr);
		char* alc1 = (char*)calloc(strlen(tablestr) - index, 1);
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++) {
			alc1[i] = tablestr[index + i];
		}
		char* alc2 = (char*)calloc(pindex + 1, 1);
		for (unsigned long long i = 0; i < pindex; i++) {
			alc2[i] = tablestr[index - pindex + i];
		}
		if ((filesize - size) % sectorsize == 0) { // Dealloc entire block out of alc2
			for (unsigned long long i = 0; i < pindex + 1; i++) {
				if ((alc2[pindex - i] & 0xff) == 44) {
					alc2[pindex - i] = 0;
					break;
				}
				alc2[pindex - i] = 0;
			}
		}
		else if (filesize % sectorsize == 0) { // Realloc full block as part block
			unsigned long long alc2len = strlen(alc2);
			char part[4] = ";0;";
			for (unsigned long long i = 0; i < 3; i++) {
				alc2[alc2len + i] = part[i];
			}
			for (unsigned long long i = 0; i < strlen(std::to_string(sectorsize - size % sectorsize).c_str()); i++) {
				alc2[alc2len + 3 + i] = std::to_string(sectorsize - size % sectorsize).c_str()[i];
			}
			alc2[alc2len + 3 + strlen(std::to_string(sectorsize - size % sectorsize).c_str())] = 0;
		}
		else { // Realloc part block as smaller part block
			unsigned long long alc2partlen = 0;
			for (; alc2partlen < strlen(alc2); alc2partlen++) {
				if ((alc2[strlen(alc2) - alc2partlen] & 0xff) == 59) {
					break;
				}
			}
			unsigned long alc2part = std::strtoul(alc2 + strlen(alc2) - alc2partlen + 1, 0, 10);
			for (unsigned long long i = 0; i < strlen(std::to_string(alc2part - size % sectorsize).c_str()); i++) {
				alc2[strlen(alc2) - alc2partlen + 1 + i] = std::to_string(alc2part - size % sectorsize)[i];
			}
			alc2[strlen(alc2) - alc2partlen + strlen(std::to_string(alc2part - size % sectorsize).c_str()) + 1] = 0;
		}
		unsigned long long off = 0;
		for (; off < strlen(alc2); off++) {
			if ((alc2[off] & 0xff) == 0) {
				break;
			}
			tablestr[index - pindex + off] = alc2[off];
		}
		for (unsigned long long i = 0; i < strlen(alc1); i++) {
			tablestr[index - pindex + off + i] = alc1[i];
		}
		cleantablestr(charmap, tablestr);
		index = index - pindex + off;
		filesize -= size % sectorsize;
	}
	for (unsigned long long p = 0; p < size / sectorsize; p++) { // Dealloc entire block out of alc2
		unsigned long long pindex = getpindex(index, tablestr);
		char* alc1 = (char*)calloc(strlen(tablestr) - index, 1);
		for (unsigned long long i = 0; i < strlen(tablestr) - index; i++) {
			alc1[i] = tablestr[index + i];
		}
		char* alc2 = (char*)calloc(pindex + 1, 1);
		for (unsigned long long i = 0; i < pindex; i++) {
			alc2[i] = tablestr[index - pindex + i];
		}
		for (unsigned long long i = 0; i < pindex + 1; i++) {
			if ((alc2[pindex - i] & 0xff) == 44) {
				alc2[pindex - i] = 0;
				break;
			}
			alc2[pindex - i] = 0;
		}
		unsigned long long off = 0;
		for (; off < strlen(alc2); off++) {
			if ((alc2[off] & 0xff) == 0) {
				break;
			}
			tablestr[index - pindex + off] = alc2[off];
		}
		for (unsigned long long i = 0; i < strlen(alc1); i++) {
			tablestr[index - pindex + off + i] = alc1[i];
		}
		cleantablestr(charmap, tablestr);
		index = index - pindex + off;
		filesize -= sectorsize;
	}
	return 0;
}

unsigned long long getfilenameindex(PWSTR filename, char* filenames, char* tablestr, unsigned long long filenamecount) {
	unsigned long long p = 0;
	unsigned long long o = 0;
	for (; o < filenamecount; o++) {
		char file[256] = { 0 };
		unsigned i = 0;
		for (; i < 256; i++) {
			if ((filenames[p + i] & 0xff) == 255 || (filenames[p + i] & 0xff) == 42) {
				p += i + 1;
				break;
			}
			file[i] = filenames[p + i];
		}
		if (strcmp(file, (char*)filename) == 0) {
			break;
		}
	}
	unsigned long long index = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++) {
		if ((tablestr[i] & 0xff) == 46) {
			if (index == o) {
				return i;
			}
			index++;
		}
	}
}

int simptable(HANDLE& hDisk, unsigned long sectorsize, unsigned long& tablesize, unsigned long& extratablesize, unsigned long long filenamecount, char*& fileinfo, char*& filenames, char*& tablestr, char*& table, std::map<unsigned, unsigned> emap, std::map<unsigned, unsigned> dmap) {
	_LARGE_INTEGER seek = { 0 };
	SetFilePointerEx(hDisk, seek, NULL, 0);
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++) {
		if ((tablestr[i] & 0xff) == 46) {
			tablelen = i + 1;
		}
	}
	encode(emap, tablestr, tablelen);
	tablelen /= 2;
	unsigned long long filenamesizes = 0;
	for (unsigned long long i = 0; i < strlen(filenames); i++) {
		if ((filenames[i] & 0xff) == 255) {
			filenamesizes = i + 1;
		}
	}
	tablesize = (tablelen + filenamesizes + (35 * filenamecount) + sectorsize - 1) / sectorsize;
	settablesize(sectorsize, tablesize, extratablesize, table);
	for (unsigned long long i = 0; i < tablelen; i++) {
		table[i + 5] = (unsigned)tablestr[i] & 0xff;
	}
	table[tablelen + 5] = 255;
	for (unsigned long long i = 0; i < filenamesizes; i++) {
		table[tablelen + i + 6] = filenames[i];
	}
	table[tablelen + 6 + filenamesizes] = 254;
	for (unsigned long long i = 0; i < filenamecount; i++) {
		for (unsigned long long j = 0; j < 35; j++) {
			table[tablelen + filenamesizes + 7 + (i * 35) + j] = fileinfo[(i * 35) + j];
		}
	}
	decode(dmap, tablestr, tablelen);
	DWORD w;
	WriteFile(hDisk, table, ((tablelen + filenamesizes + 7 + (filenamecount * 35) + 511) / 512) * 512, &w, NULL);
	if (w != ((tablelen + filenamesizes + 7 + (filenamecount * 35) + 511) / 512) * 512) {
		return 1;
	}
	return 0;
}

int createfile(PWSTR filename, unsigned mode, unsigned long long& filenamecount, char*& fileinfo, char*& filenames, char* charmap, char*& tablestr) {
	char* alc = (char*)realloc(fileinfo, (filenamecount + 1) * 35);
	if (alc == NULL) {
		return 1;
	}
	fileinfo = alc;
	char* file = NULL;
	unsigned long long oldlen = 0;
	for (unsigned long long i = 0; i < strlen(filenames); i++) {
		if ((filenames[i] & 0xff) == 255) {
			oldlen = i + 1;
		}
	}
	file = ((char*)filename);
	strcpy_s(filenames + oldlen, strlen(file) + 1, file);
	filenames[oldlen + strlen(file)] = 255;
	unsigned long long tablelen = 0;
	for (unsigned long long i = 0; i < strlen(tablestr); i++) {
		if ((tablestr[i] & 0xff) == 46) {
			tablelen = i;
		}
	}
	if (tablelen == 0) {
		tablestr[tablelen] = 46;
	}
	else {
		tablestr[tablelen + 1] = 46;
	}
	char* gum = (char*)calloc((filenamecount + 1) * 11, 1);
	memcpy(gum, fileinfo + filenamecount * 24, filenamecount * 11);
	time_t ltime;
	time(&ltime);
	double t = (double)ltime;
	char ti[8] = { 0 };
	memcpy(ti, &t, 8);
	char tim[8] = { 0 };
	for (unsigned i = 0; i < 8; i++) {
		tim[i] = ti[7 - i];
	}
	memcpy(fileinfo + filenamecount * 24, &tim, 8);
	memcpy(fileinfo + filenamecount * 24 + 8, &tim, 8);
	memcpy(fileinfo + filenamecount * 24 + 16, &tim, 8);
	char guidmodes[11] = { 0 };
	unsigned gid;
	unsigned uid;
	unsigned winattrs;
	gid = uid = 545;
	winattrs = 2048;
	guidmodes[0] = (gid >> 16) & 0xff;
	guidmodes[1] = (gid >> 8) & 0xff;
	guidmodes[2] = gid & 0xff;
	guidmodes[3] = (uid >> 8) & 0xff;
	guidmodes[4] = uid & 0xff;
	guidmodes[5] = (mode >> 8) & 0xff;
	guidmodes[6] = mode & 0xff;
	guidmodes[7] = (winattrs >> 24) & 0xff;
	guidmodes[8] = (winattrs >> 16) & 0xff;
	guidmodes[9] = (winattrs >> 8) & 0xff;
	guidmodes[10] = winattrs & 0xff;
	for (unsigned i = 0; i < 11; i++) {
		gum[filenamecount * 11 + i] = guidmodes[i];
	}
	for (unsigned long long i = 0; i < (filenamecount + 1) * 11; i++) {
		fileinfo[(filenamecount + 1) * 24 + i] = gum[i];
	}
	free(gum);
	filenamecount++;
	cleantablestr(charmap, tablestr);
	return 0;
}

void readwrite(HANDLE hDisk, unsigned long sectorsize, unsigned long long disksize, unsigned long long start, unsigned step, unsigned range, unsigned long long len, std::string str0, std::string str1, std::string str2, std::string rstr, unsigned long long& rblock, unsigned long long& block, char*& buf, unsigned rw) {
	LARGE_INTEGER loc;
	DWORD wr;
	if (range != 0) {
		for (unsigned long long p = std::strtoull(rstr.c_str(), 0, 10); p < std::strtoull(str0.c_str(), 0, 10) + 1; p++) {
			if (start / sectorsize <= block && block < (start + len + sectorsize - 1) / sectorsize) {
				if (start / sectorsize == block) { // Start block
					if (step != 0) {
						loc.QuadPart = disksize - (p * sectorsize + sectorsize) + start % sectorsize + std::strtoul(str1.c_str(), 0, 10);
						SetFilePointerEx(hDisk, loc, NULL, 0);
						if (len - start % sectorsize < std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10)) {
							if (rw == 0) {
								ReadFile(hDisk, buf, len, &wr, NULL);
							}
							else {
								WriteFile(hDisk, buf, len, &wr, NULL);
							}
							rblock += len;
						}
						else {
							if (rw == 0) {
								ReadFile(hDisk, buf, std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL);
							}
							else {
								WriteFile(hDisk, buf, std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL);
							}
							rblock += std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize;
						}
					}
					else {
						loc.QuadPart = disksize - (p * sectorsize + sectorsize) + start % sectorsize;
						SetFilePointerEx(hDisk, loc, NULL, 0);
						if (sectorsize - start % sectorsize < sectorsize) {
							if (rw == 0) {
								ReadFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
							}
							else {
								WriteFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
							}
							rblock += sectorsize - start % sectorsize;
						}
						else {
							if (rw == 0) {
								ReadFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
							}
							else {
								WriteFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
							}
							rblock += sectorsize - start % sectorsize;
						}
					}
				}
				else if (len - rblock <= sectorsize) { // End block
					if (step != 0) {
						loc.QuadPart = disksize - (p * sectorsize + sectorsize) + std::strtoul(str1.c_str(), 0, 10);
					}
					else {
						loc.QuadPart = disksize - (p * sectorsize + sectorsize);
					}
					SetFilePointerEx(hDisk, loc, NULL, 0);
					if (rw == 0) {
						ReadFile(hDisk, buf + rblock, len - rblock, &wr, NULL);
					}
					else {
						WriteFile(hDisk, buf + rblock, len - rblock, &wr, NULL);
					}
					rblock = len;
				}
				else { // In between blocks
					loc.QuadPart = disksize - (p * sectorsize + sectorsize);
					SetFilePointerEx(hDisk, loc, NULL, 0);
					if (rw == 0) {
						ReadFile(hDisk, buf + rblock, sectorsize, &wr, NULL);
					}
					else {
						WriteFile(hDisk, buf + rblock, sectorsize, &wr, NULL);
					}
					rblock += sectorsize;
				}
			}
			block++;
		}
		block--;
		return;
	}
	if (start / sectorsize <= block && block < (start + len + sectorsize - 1) / sectorsize) {
		if (start / sectorsize == block) { // Start block
			if (step != 0) {
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize) + start % sectorsize + std::strtoul(str1.c_str(), 0, 10);
				SetFilePointerEx(hDisk, loc, NULL, 0);
				if (len - start % sectorsize < std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10)) {
					if (rw == 0) {
						ReadFile(hDisk, buf, len, &wr, NULL);
					}
					else {
						WriteFile(hDisk, buf, len, &wr, NULL);
					}
					rblock += len;
				}
				else {
					if (rw == 0) {
						ReadFile(hDisk, buf, std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL);
					}
					else {
						WriteFile(hDisk, buf, std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize, &wr, NULL);
					}
					rblock += std::strtoul(str2.c_str(), 0, 10) - std::strtoul(str1.c_str(), 0, 10) - start % sectorsize;
				}
			}
			else {
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize) + start % sectorsize;
				SetFilePointerEx(hDisk, loc, NULL, 0);
				if (sectorsize - start % sectorsize < sectorsize) {
					if (rw == 0) {
						ReadFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
					}
					else {
						WriteFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
					}
					rblock += sectorsize - start % sectorsize;
				}
				else {
					if (rw == 0) {
						ReadFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
					}
					else {
						WriteFile(hDisk, buf, sectorsize - start % sectorsize, &wr, NULL);
					}
					rblock += sectorsize - start % sectorsize;
				}
			}
		}
		else if (len - rblock <= sectorsize) { // End block
			if (step != 0) {
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize) + std::strtoul(str1.c_str(), 0, 10);
			}
			else {
				loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize);
			}
			SetFilePointerEx(hDisk, loc, NULL, 0);
			if (rw == 0) {
				ReadFile(hDisk, buf + rblock, len - rblock, &wr, NULL);
			}
			else {
				WriteFile(hDisk, buf + rblock, len - rblock, &wr, NULL);
			}
			rblock = len;
		}
		else { // In between blocks
			loc.QuadPart = disksize - (std::strtoull(str0.c_str(), 0, 10) * sectorsize + sectorsize);
			SetFilePointerEx(hDisk, loc, NULL, 0);
			if (rw == 0) {
				ReadFile(hDisk, buf + rblock, sectorsize, &wr, NULL);
			}
			else {
				WriteFile(hDisk, buf + rblock, sectorsize, &wr, NULL);
			}
			rblock += sectorsize;
		}
	}
}

int readwritefile(HANDLE hDisk, unsigned long long sectorsize, unsigned long long index, unsigned long long start, unsigned long long len, unsigned long long disksize, char* tablestr, char*& buf, unsigned rw) {
	unsigned long long pindex = getpindex(index, tablestr);
	unsigned long long filesize = 0;
	getfilesize(sectorsize, index, tablestr, filesize);
	len = min(len + start, filesize - start);
	index++;
	pindex++;
	char* alc = (char*)calloc(pindex + 1, 1);
	for (unsigned long long i = 0; i < pindex; i++) {
		alc[i] = tablestr[index - pindex + i];
	}
	unsigned long long block = 0;
	unsigned long long rblock = 0;
	char* alc1 = NULL;
	unsigned long long clen = 256;
	char* cblock = (char*)calloc(clen, 1);
	unsigned long long cloc = 0;
	std::string str0;
	std::string str1;
	std::string str2;
	std::string rstr;
	unsigned step = 0;
	unsigned range = 0;
	for (unsigned long long i = 0; i < pindex; i++) {
		switch (alc[i] & 0xff) {
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
			if (cloc > clen - 2) {
				clen += 256;
				alc1 = (char*)realloc(cblock, clen);
				if (alc1 == NULL) {
					free(cblock);
					return 1;
				}
				cblock = alc1;
				alc1 = NULL;
			}
			cblock[cloc] = alc[i];
			cloc++;
			break;
		}
	}
	return 0;
}

int trunfile(HANDLE hDisk, unsigned long sectorsize, unsigned long long index, unsigned long tablesize, unsigned long long disksize, unsigned long long size, unsigned long long newsize, char* charmap, char*& tablestr) {
	if (size < newsize) {
		if (size % sectorsize != 0) {
			char* temp = (char*)calloc(size % sectorsize, 1);
			readwritefile(hDisk, sectorsize, index, size - size % sectorsize, size % sectorsize, disksize, tablestr, temp, 0);
			dealloc(sectorsize, charmap, tablestr, index, size, size % sectorsize);
			alloc(sectorsize, disksize, tablesize, charmap, tablestr, index, newsize - (size - size % sectorsize));
			readwritefile(hDisk, sectorsize, index, size - size % sectorsize, size % sectorsize, disksize, tablestr, temp, 1);
			size += newsize - size;
		}
		alloc(sectorsize, disksize, tablesize, charmap, tablestr, index, newsize - size);
	}
	if (size > newsize) {
		if (size % sectorsize != 0 && size - newsize > size % sectorsize) {
			dealloc(sectorsize, charmap, tablestr, index, size, size % sectorsize);
			size -= size % sectorsize;
		}
		dealloc(sectorsize, charmap, tablestr, index, size, size - newsize);
	}
	return 0;
}

int main(int argc, char* argv[]) {
	if (argc == 1) {
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
	if (hDisk == INVALID_HANDLE_VALUE) {
		std::cout << "Opening Error: " << GetLastError() << std::endl;
		return 1;
	}
	//std::cout << "Disk opened successfully" << std::endl;

	if (argc == 3) {
		sscanf_s(argv[2], "%d", &sectorsize);
		unsigned i = 0;
		while (sectorsize != 1) {
			sectorsize >>= 1;
			i++;
		}
		if (i < 9) {
			i = 9;
		}
		i -= 9;
		char bytes[512] = { 0 };
		bytes[0] = i;
		bytes[5] = 255;
		bytes[6] = 254;
		DWORD w;
		WriteFile(hDisk, bytes, 512, &w, NULL);
		if (w != 512) {
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
	ReadFile(hDisk, bytes, 512, &r, NULL);
	if (r != 512) {
		std::cout << "Reading Error: " << GetLastError() << std::endl;
		return 1;
	}
	sectorsize = pow(2, 9 + bytes[0]);
	//std::cout << "Read disk with sectorsize: " << sectorsize << std::endl;

	unsigned long tablesize = 1 + bytes[4] + (bytes[3] << 8) + (bytes[2] << 16) + (bytes[1] << 24);
	unsigned long extratablesize = (tablesize * sectorsize) - 512;
	char* ttable = (char*)calloc(extratablesize, 1);
	ReadFile(hDisk, ttable, extratablesize, &r, NULL);
	if (r != extratablesize) {
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
	for (unsigned i = 0; i < 15; i++) {
		for (unsigned o = 0; o < 15; o++) {
			c = charmap[i] << 8 | charmap[o];
			emap[c] = p;
			dmap[p] = c;
			p++;
		}
	}

	unsigned long long pos = 0;
	while (((unsigned)table[pos] & 0xff) != 255) {
		pos++;
	}
	//std::cout << "Found end of table at: " << pos << std::endl;
	//std::cout << "Table size: " << pos - 5 << std::endl;
	char* tablestr = (char*)calloc(pos - 5, 1);
	memcpy(tablestr, table + 5, pos - 5);
	decode(dmap, tablestr, pos - 5);
	//std::cout << "Decoded table: " << std::string(str, (pos - 5) * 2) << std::endl;

	//encode(emap, str, (pos - 5) * 2);
	//std::cout << "Encoded table: ";
	//for (unsigned long long i = 0; i < pos - 5; i++) {
	//	std::cout << ((unsigned)str[i] & 0xff) << " ";
	//}
	//std::cout << std::endl;

	unsigned long long filenamepos = pos;
	while (((unsigned)table[filenamepos] & 0xff) != 254) {
		filenamepos++;
	}
	//std::cout << "Found end of filenames at: " << filenamepos << std::endl;
	//std::cout << "Filenames size: " << filenamepos - pos - 1 << std::endl;
	char* filenames = (char*)calloc(filenamepos - pos - 1, 1);
	memcpy(filenames, table + pos + 1, filenamepos - pos - 1);
	unsigned long long filenamecount = 0;
	for (unsigned long long i = 0; i < filenamepos - pos - 1; i++) {
		if (((unsigned)filenames[i] & 0xff) == 255) {
			filenamecount++;
		}
	}
	//std::cout << "Filename count: " << filenamecount << std::endl;
	//std::cout << "Filenames: " << std::string(filenames, filenamepos - pos - 1) << std::endl;

	char* fileinfo = (char*)calloc(filenamecount, 35);
	memcpy(fileinfo, table + filenamepos + 1, filenamecount * 35);
	//std::cout << "Fileinfo size: " << filenamecount * 35 << std::endl;
	//std::cout << "Fileinfo: " << std::string(fileinfo, filenamecount * 35) << std::endl;

	createfile((PWSTR) "/Test.bin", 448, filenamecount, fileinfo, filenames, charmap, tablestr);
	unsigned long long index = getfilenameindex((PWSTR)"/Test.bin", filenames, tablestr, filenamecount);
	alloc(sectorsize, disksize.QuadPart, tablesize, charmap, tablestr, index, 2097152*3+512);
	unsigned long long filesize = 0;
	getfilesize(sectorsize, index, tablestr, filesize);
	std::cout << filesize << " " << tablestr << std::endl;
	dealloc(sectorsize, charmap, tablestr, index, filesize, 2097152*2+512);
	getfilesize(sectorsize, index, tablestr, filesize);
	std::cout << filesize << " " << tablestr << std::endl;
	alloc(sectorsize, disksize.QuadPart, tablesize, charmap, tablestr, index, 512);
	getfilesize(sectorsize, index, tablestr, filesize);
	std::cout << filesize << " " << tablestr << std::endl;
	simptable(hDisk, sectorsize, tablesize, extratablesize, filenamecount, fileinfo, filenames, tablestr, table, emap, dmap);
	char* buf = (char*)calloc(2097152*3, 1);
	readwritefile(hDisk, sectorsize, index, 0, 2097152*3, disksize.QuadPart, tablestr, buf, 1);
	std::cout << tablestr << std::endl;
	trunfile(hDisk, sectorsize, index, tablesize, disksize.QuadPart, filesize, 2097152*2, charmap, tablestr);
	std::cout << tablestr << std::endl;

	return 0;
}