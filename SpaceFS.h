#pragma once

#include <windows.h>
#include <stdio.h>
#include <iostream>
#include <unordered_map>
#include <time.h>
#include <string>

void handmaps(std::unordered_map<unsigned, unsigned> Emap, std::unordered_map<unsigned, unsigned> Dmap, std::unordered_map<std::wstring, unsigned long long>& filenameindexlist);
void encode(char*& str, unsigned long long& len);
void decode(char*& bytes, unsigned long long len);
int settablesize(unsigned long sectorsize, unsigned long& tablesize, unsigned long long& extratablesize, char*& table);
void resetcloc(unsigned long long& cloc, std::string& cblock, std::string& str0, std::string& str1, std::string& str2, unsigned step);
unsigned long long getpindex(unsigned long long index, char* tablestr);
int getfilesize(unsigned long sectorsize, unsigned long long index, char* tablestr, unsigned long long& filesize);
void addtopartlist(unsigned long sectorsize, unsigned range, unsigned step, std::string str0, std::string str1, std::string str2, std::string rstr, unsigned long long& usedblocks);
int findblock(unsigned long sectorsize, unsigned long long disksize, unsigned long tablesize, char* tablestr, char*& block, unsigned long long& blockstrlen, unsigned long blocksize, unsigned long long& usedblocks);
int alloc(unsigned long sectorsize, unsigned long long disksize, unsigned long tablesize, char* charmap, char*& tablestr, unsigned long long& index, unsigned long long size, unsigned long long& usedblocks);
int dealloc(unsigned long sectorsize, char* charmap, char*& tablestr, unsigned long long& index, unsigned long long filesize, unsigned long long size);
void getfilenameindex(PWSTR filename, char* filenames, unsigned long long filenamecount, unsigned long long& filenameindex, unsigned long long& filenamestrindex);
unsigned long long gettablestrindex(PWSTR filename, char* filenames, char* tablestr, unsigned long long filenamecount);
int desimp(char* charmap, char*& tablestr);
int simp(char* charmap, char*& tablestr);
int simptable(HANDLE hDisk, unsigned long sectorsize, char* charmap, unsigned long& tablesize, unsigned long long& extratablesize, unsigned long long filenamecount, char*& fileinfo, char*& filenames, char*& tablestr, char*& table);
int createfile(PWSTR filename, unsigned long gid, unsigned long uid, unsigned long mode, unsigned long winattrs, unsigned long long& filenamecount, char*& fileinfo, char*& filenames, char* charmap, char*& tablestr);
int deletefile(unsigned long long index, unsigned long long filenameindex, unsigned long long filenamestrindex, unsigned long long& filenamecount, char*& fileinfo, char*& filenames, char*& tablestr);
int renamefile(PWSTR oldfilename, PWSTR newfilename, unsigned long long& filenamestrindex, char*& filenames);
unsigned readwritedrive(HANDLE hDisk, char*& buf, unsigned long long len, unsigned rw, LARGE_INTEGER loc);
void readwrite(HANDLE hDisk, unsigned long sectorsize, unsigned long long disksize, unsigned long long start, unsigned step, unsigned range, unsigned long long len, std::string str0, std::string str1, std::string str2, std::string rstr, unsigned long long& rblock, unsigned long long& block, char*& buf, unsigned rw);
void chtime(char*& fileinfo, unsigned long long filenameindex, double& time, unsigned ch);
void chgid(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& gid, unsigned ch);
void chuid(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& uid, unsigned ch);
void chmode(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& mode, unsigned ch);
void chwinattrs(char*& fileinfo, unsigned long long filenamecount, unsigned long long filenameindex, unsigned long& winattrs, unsigned ch);
int readwritefile(HANDLE hDisk, unsigned long long sectorsize, unsigned long long index, unsigned long long start, unsigned long long len, unsigned long long disksize, char* tablestr, char*& buf, char*& fileinfo, unsigned long long filenameindex, unsigned rw);
int trunfile(HANDLE hDisk, unsigned long sectorsize, unsigned long long& index, unsigned long tablesize, unsigned long long disksize, unsigned long long size, unsigned long long newsize, unsigned long long filenameindex, char* charmap, char*& tablestr, char*& fileinfo, unsigned long long& usedblocks, PWSTR filename, char* filenames, unsigned long long filenamecount);
