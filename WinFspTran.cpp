#include <winfsp/winfsp.h>
#include <sddl.h>
#include "SpaceFS.h"

#define PROGNAME PWSTR(L"CSpaceFS")

#define FULLPATH_SIZE (MAX_PATH + FSP_FSCTL_TRANSACT_PATH_SIZEMAX / sizeof(WCHAR))

#define info(format, ...) FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...) FspServiceLog(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...) FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

char* charmap = (char*)"0123456789-,.; ";
std::unordered_map<std::wstring, unsigned long long> opened = {};
std::unordered_map<std::wstring, unsigned long long> allocationsizes = {};
std::unordered_map<std::wstring, unsigned long long> filenameindexlist = {};

typedef struct
{
	FSP_FILE_SYSTEM* FileSystem;
	PWSTR MountPoint;
	HANDLE hDisk;
	ULONG SectorSize;
	ULONGLONG DiskSize;
	ULONG TableSize;
	ULONGLONG ExtraTableSize;
	char* Table;
	char* TableStr;
	ULONGLONG FilenameCount;
	char* Filenames;
	char* FileInfo;
	ULONGLONG UsedBlocks;
} SPFS;

typedef struct
{
	PWSTR Path;
} SPFS_FILE_CONTEXT;

DWORD SetSecurityDescriptor(PSECURITY_DESCRIPTOR pInDescriptor, SECURITY_INFORMATION iSecInfo, PSECURITY_DESCRIPTOR pModDescriptor, PSECURITY_DESCRIPTOR* ppOutDescriptor)
{ // Thank you PuckyBoy for this code.
	DWORD iDescriptorSize = 0;
	DWORD iDaclSize = 0;
	DWORD iSaclSize = 0;
	DWORD iOwnerSize = 0;
	DWORD iGroupSize = 0;

	if (iSecInfo & (ATTRIBUTE_SECURITY_INFORMATION | BACKUP_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION)) return ERROR_INVALID_SECURITY_DESCR; // Don't know how to handle these flags

	if (!MakeAbsoluteSD(pInDescriptor, NULL, &iDescriptorSize, NULL, &iDaclSize, NULL, &iSaclSize, NULL, &iOwnerSize, NULL, &iGroupSize) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return GetLastError();

	DWORD iSize = iDescriptorSize + iDaclSize + iSaclSize + iOwnerSize + iGroupSize;

	PUCHAR pBuff = (PUCHAR)malloc(iSize);
	if (pBuff == NULL) return ERROR_OUTOFMEMORY;

	PSECURITY_DESCRIPTOR pAbsDescriptor = (PSECURITY_DESCRIPTOR)pBuff;
	PACL pAbsDacl = (PACL)(pBuff + iDescriptorSize);
	PACL pAbsSacl = (PACL)(pBuff + iDescriptorSize + iDaclSize);
	PSID pAbsOwner = (PSID)(pBuff + iDescriptorSize + iDaclSize + iSaclSize);
	PSID pAbsGroup = (PSID)(pBuff + iDescriptorSize + iDaclSize + iSaclSize + iOwnerSize);

	if (!MakeAbsoluteSD(pInDescriptor, pAbsDescriptor, &iDescriptorSize, pAbsDacl, &iDaclSize, pAbsSacl, &iSaclSize, pAbsOwner, &iOwnerSize, pAbsGroup, &iGroupSize))
	{
		free(pBuff);
		return GetLastError();
	}

	if (iSecInfo & DACL_SECURITY_INFORMATION)
	{
		BOOL bPresent;
		PACL pAcl;
		BOOL bDefault;

		if (!GetSecurityDescriptorDacl(pModDescriptor, &bPresent, &pAcl, &bDefault))
		{
			free(pBuff);
			return GetLastError();
		}

		if (bPresent && pAcl && !IsValidAcl(pAcl))
		{
			free(pBuff);
			return ERROR_INVALID_ACL;
		}

		if (!SetSecurityDescriptorDacl(pAbsDescriptor, bPresent, pAcl, bDefault))
		{
			free(pBuff);
			return GetLastError();
		}
	}

	if (iSecInfo & SACL_SECURITY_INFORMATION)
	{
		BOOL bPresent;
		PACL pAcl;
		BOOL bDefault;

		if (!GetSecurityDescriptorSacl(pModDescriptor, &bPresent, &pAcl, &bDefault))
		{
			free(pBuff);
			return GetLastError();
		}

		if (bPresent && pAcl && !IsValidAcl(pAcl))
		{
			free(pBuff);
			return ERROR_INVALID_ACL;
		}

		if (!SetSecurityDescriptorSacl(pAbsDescriptor, bPresent, pAcl, bDefault))
		{
			free(pBuff);
			return GetLastError();
		}
	}

	if (iSecInfo & OWNER_SECURITY_INFORMATION)
	{
		PSID pSid;
		BOOL bDefault;

		if (!GetSecurityDescriptorOwner(pModDescriptor, &pSid, &bDefault))
		{
			free(pBuff);
			return GetLastError();
		}

		if (pSid && !IsValidSid(pSid))
		{
			free(pBuff);
			return ERROR_INVALID_SID;
		}

		if (!SetSecurityDescriptorOwner(pAbsDescriptor, pSid, bDefault))
		{
			free(pBuff);
			return GetLastError();
		}
	}

	if (iSecInfo & GROUP_SECURITY_INFORMATION)
	{
		PSID pSid;
		BOOL bDefault;

		if (!GetSecurityDescriptorGroup(pModDescriptor, &pSid, &bDefault))
		{
			free(pBuff);
			return GetLastError();
		}

		if (pSid && !IsValidSid(pSid))
		{
			free(pBuff);
			return ERROR_INVALID_SID;
		}

		if (!SetSecurityDescriptorGroup(pAbsDescriptor, pSid, bDefault))
		{
			free(pBuff);
			return GetLastError();
		}
	}

	SECURITY_DESCRIPTOR_CONTROL iMaskControl = 0;
	SECURITY_DESCRIPTOR_CONTROL iControl = 0;

	if (iSecInfo & (PROTECTED_DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION))
	{
		iMaskControl |= SE_DACL_PROTECTED;
		iControl |= (iSecInfo & PROTECTED_DACL_SECURITY_INFORMATION) ? SE_DACL_PROTECTED : 0;
	}

	if (iSecInfo & (PROTECTED_SACL_SECURITY_INFORMATION | UNPROTECTED_SACL_SECURITY_INFORMATION))
	{
		iMaskControl |= SE_SACL_PROTECTED;
		iControl |= (iSecInfo & PROTECTED_SACL_SECURITY_INFORMATION) ? SE_SACL_PROTECTED : 0;
	}

	if (iMaskControl && !SetSecurityDescriptorControl(pAbsDescriptor, iMaskControl, iControl))
	{
		free(pBuff);
		return GetLastError();
	}

	iSize = GetSecurityDescriptorLength(pAbsDescriptor);

	PSECURITY_DESCRIPTOR pOutDescriptor = (PSECURITY_DESCRIPTOR)malloc(iSize);
	if (pOutDescriptor == NULL)
	{
		free(pBuff);
		return ERROR_OUTOFMEMORY;
	}

	if (!MakeSelfRelativeSD(pAbsDescriptor, pOutDescriptor, &iSize))
	{
		free(pOutDescriptor);
		free(pBuff);
		return GetLastError();
	}

	free(pBuff);
	*ppOutDescriptor = pOutDescriptor;
	return ERROR_SUCCESS;
}

static VOID attrtoATTR(unsigned long& attr)
{
	unsigned long ATTR = 0;
	if (attr & FILE_ATTRIBUTE_HIDDEN) ATTR |= 32768;
	if (attr & FILE_ATTRIBUTE_READONLY) ATTR |= 4096;
	if (attr & FILE_ATTRIBUTE_SYSTEM) ATTR |= 128;
	if (attr & FILE_ATTRIBUTE_ARCHIVE) ATTR |= 2048;
	if (attr & FILE_ATTRIBUTE_DIRECTORY) ATTR |= 8192;
	if (attr & FILE_ATTRIBUTE_REPARSE_POINT) ATTR |= 1024;
	attr = ATTR;
}

static VOID ATTRtoattr(unsigned long& ATTR)
{
	unsigned long attr = 0;
	if (ATTR & 32768) attr |= FILE_ATTRIBUTE_HIDDEN;
	if (ATTR & 4096) attr |= FILE_ATTRIBUTE_READONLY;
	if (ATTR & 128) attr |= FILE_ATTRIBUTE_SYSTEM;
	if (ATTR & 2048) attr |= FILE_ATTRIBUTE_ARCHIVE;
	if (ATTR & 8192) attr |= FILE_ATTRIBUTE_DIRECTORY;
	if (ATTR & 1024) attr |= FILE_ATTRIBUTE_REPARSE_POINT;
	ATTR = attr;
}

static VOID ReplaceBSWFS(PWSTR& FileName)
{
	unsigned long long FileNameLen = wcslen(FileName);
	wchar_t* NewFileName = (wchar_t*)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	unsigned long long i = 0;
	for (; i < FileNameLen; i++)
	{
		if (FileName[i] == '\\')
		{
			NewFileName[i] = '/';
		}
		else
		{
			NewFileName[i] = FileName[i];
		}
		if (FileName[i] == '\0')
		{
			break;
		}
	}

	memcpy(FileName, NewFileName, (i + 1) * sizeof(wchar_t));
	free(NewFileName);
}

static VOID RemoveFirst(PWSTR& FileName)
{
	unsigned long long FileNameLen = wcslen(FileName);
	wchar_t* NewFileName = (wchar_t*)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	unsigned long long i = 0;
	for (; i < FileNameLen; i++)
	{
		NewFileName[i] = FileName[i + 1];
		if (FileName[i + 1] == '\0')
		{
			break;
		}
	}

	memcpy(FileName, NewFileName, (i + 1) * sizeof(wchar_t));
	free(NewFileName);
}

static VOID RemoveStream(PWSTR& FileName, PWSTR& Suffix)
{
	unsigned long long FileNameLen = wcslen(FileName);
	for (unsigned long long i = 0; i < FileNameLen; i++)
	{
		if (FileName[i] == ':')
		{
			FileName[i] = '\0';
			Suffix = FileName + i + 1;
			break;
		}
		if (FileName[i] == '\0')
		{
			FileName[i] = '\0';
			break;
		}
	}
}

static VOID GetParentName(PWSTR& FileName, PWSTR& Suffix)
{
	unsigned Loc = 0;
	unsigned long long FileNameLen = wcslen(FileName);
	wchar_t NewFileName[512] = { 0 };

	for (unsigned i = 0; i < FileNameLen; i++)
	{
		if (FileName[i] == '\0')
		{
			break;
		}
		switch (FileName[i])
		{
		case '/':
			NewFileName[i] = FileName[i];
			Loc = i;
			break;
		default:
			NewFileName[i] = FileName[i];
			break;
		}
	}

	if (Suffix)
	{
		wchar_t NewSuffix[512] = { 0 };

		unsigned i = 0;
		for (; i < FileNameLen; i++)
		{
			if (FileName[Loc + i + 1] == '\0')
			{
				break;
			}
			NewSuffix[i] = FileName[Loc + i + 1];
		}

		memcpy(Suffix, NewSuffix, (static_cast<unsigned long long>(i) + 1) * sizeof(wchar_t));
	}

	if (Loc)
	{
		NewFileName[Loc] = '\0';
	}
	NewFileName[Loc + 1] = '\0';
	memcpy(FileName, NewFileName, (static_cast<unsigned long long>(Loc) + 2) * sizeof(wchar_t));
}

static NTSTATUS FindDuplicate(SPFS* SpFs, PWSTR FileName)
{
	if (filenameindexlist[std::wstring(FileName)] != 0)
	{
		return STATUS_OBJECT_NAME_COLLISION;
	}
	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileName), FileNameLenT = max(FileNameLen, 0xff);
	PWSTR ALC = NULL;
	PWSTR Filename = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(Filename, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				Filename = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			Filename[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		Filename[j] = 0;

		if (!_wcsicmp(Filename, FileName) && wcslen(Filename) == FileNameLen)
		{
			free(Filename);
			return STATUS_OBJECT_NAME_COLLISION;
		}
	}

	free(Filename);
	return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfoInternal(SPFS* SpFs, FSP_FSCTL_FILE_INFO* FileInfo, PWSTR FileName)
{
	unsigned long long Index = gettablestrindex(FileName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long FileSize = 0;
	unsigned long winattrs = 0;
	double LastAccessTime = 0;
	double LastWriteTime = 0;
	double CreationTime = 0;

	unsigned long long FileNameLen = wcslen(FileName);
	PWSTR NoStreamFileName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!NoStreamFileName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(NoStreamFileName, FileName, FileNameLen * sizeof(wchar_t));
	PWSTR Suffix = NULL;
	ReplaceBSWFS(NoStreamFileName);
	std::wstring Path = NoStreamFileName;
	RemoveStream(NoStreamFileName, Suffix);
	unsigned long long NoStreamFileNameIndex = 0;
	unsigned long long NoStreamFileNameSTRIndex = 0;
	getfilenameindex(NoStreamFileName, SpFs->Filenames, SpFs->FilenameCount, NoStreamFileNameIndex, NoStreamFileNameSTRIndex);
	free(NoStreamFileName);

	getfilenameindex(FileName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	chtime(SpFs->FileInfo, NoStreamFileNameIndex, LastAccessTime, 0);
	chtime(SpFs->FileInfo, NoStreamFileNameIndex, LastWriteTime, 2);
	chtime(SpFs->FileInfo, NoStreamFileNameIndex, CreationTime, 4);

	UINT64 CTime = CreationTime * 10000000 + 116444736000000000;
	UINT64 ATime = LastAccessTime * 10000000 + 116444736000000000;
	UINT64 WTime = LastWriteTime * 10000000 + 116444736000000000;

	ATTRtoattr(winattrs);
	FileInfo->FileAttributes = winattrs;
	if (FileInfo->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		char* buf = (char*)calloc(4, sizeof(char));
		if (!buf)
		{
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, 4, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 0);
		FileInfo->ReparseTag = *(unsigned long*)buf;
		free(buf);
	}
	else
	{
		FileInfo->ReparseTag = 0;
	}
	FileInfo->FileSize = FileSize;
	if (!allocationsizes[Path])
	{
		FileInfo->AllocationSize = (FileSize + SpFs->SectorSize - 1) / SpFs->SectorSize * SpFs->SectorSize;
	}
	else
	{
		FileInfo->AllocationSize = allocationsizes[Path];
	}
	FileInfo->CreationTime = CTime + (static_cast<unsigned long long>(2) * (CTime > 116444736000000000)) + (CTime == 279172874304) + (static_cast<unsigned long long>(3) * (CTime == 287762808896));
	FileInfo->LastAccessTime = ATime + (static_cast<unsigned long long>(2) * (ATime > 116444736000000000)) + (ATime == 279172874304) + (static_cast<unsigned long long>(3) * (ATime == 287762808896));
	FileInfo->LastWriteTime = WTime + (static_cast<unsigned long long>(2) * (WTime > 116444736000000000)) + (WTime == 279172874304) + (static_cast<unsigned long long>(3) * (WTime == 287762808896));
	FileInfo->ChangeTime = FileInfo->LastWriteTime;
	FileInfo->IndexNumber = NoStreamFileNameIndex;
	FileInfo->HardLinks = 0;
	FileInfo->EaSize = 0;

	return STATUS_SUCCESS;
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* FileSystem, FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FileSize = 0;
	unsigned long long Index = gettablestrindex(PWSTR(L":"), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	char* buf = (char*)calloc(32, 1);

	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	getfilenameindex(PWSTR(L":"), SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 0);

	VolumeInfo->TotalSize = SpFs->DiskSize - static_cast<unsigned long long>(SpFs->TableSize) * SpFs->SectorSize - SpFs->SectorSize;
	VolumeInfo->FreeSize = SpFs->DiskSize - static_cast<unsigned long long>(SpFs->TableSize) * SpFs->SectorSize - SpFs->SectorSize - SpFs->UsedBlocks * SpFs->SectorSize;
	for (int i = 0; i < FileSize; i++)
	{
		VolumeInfo->VolumeLabel[i] = buf[i];
	}
	VolumeInfo->VolumeLabelLength = FileSize * sizeof(wchar_t);
	free(buf);

	return STATUS_SUCCESS;
}

static NTSTATUS SetVolumeLabel_(FSP_FILE_SYSTEM* FileSystem, PWSTR Label, FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FileSize = 0;
	unsigned long long Index = gettablestrindex(PWSTR(L":"), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long LabelLen = wcslen(Label);

	getfilenameindex(PWSTR(L":"), SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	if (trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, LabelLen, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L":"), SpFs->Filenames, SpFs->FilenameCount))
	{
		return STATUS_DISK_FULL;
	}
	char* buf = (char*)calloc(32, 1);
	if (!buf)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	for (int i = 0; i < LabelLen; i++)
	{
		buf[i] = Label[i];
	}
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, LabelLen, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 1);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

	VolumeInfo->TotalSize = SpFs->DiskSize - static_cast<unsigned long long>(SpFs->TableSize) * SpFs->SectorSize - SpFs->SectorSize;

	unsigned long long DirSize = 0;
	FilenameIndex = 0;
	FilenameSTRIndex = 0;
	getfilenameindex(PWSTR(L"/"), SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	Index = gettablestrindex(PWSTR(L"/"), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, DirSize);
	if (!trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, DirSize, DirSize + 1, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L"/"), SpFs->Filenames, SpFs->FilenameCount))
	{
		trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, DirSize + 1, DirSize, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L"/"), SpFs->Filenames, SpFs->FilenameCount);
	}

	VolumeInfo->FreeSize = SpFs->DiskSize - static_cast<unsigned long long>(SpFs->TableSize) * SpFs->SectorSize - SpFs->SectorSize - SpFs->UsedBlocks * SpFs->SectorSize;
	memcpy(VolumeInfo->VolumeLabel, Label, LabelLen * sizeof(wchar_t));
	VolumeInfo->VolumeLabelLength = LabelLen * sizeof(wchar_t);
	free(buf);

	return STATUS_SUCCESS;
}

static NTSTATUS GetReparsePoint(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PVOID Buffer, PSIZE_T PSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FileNameLen = wcslen(FileName);
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(Filename, FileName, FileNameLen * sizeof(wchar_t));
	ReplaceBSWFS(Filename);
	FSP_FSCTL_FILE_INFO* FileInfo = (FSP_FSCTL_FILE_INFO*)calloc(sizeof(FSP_FSCTL_FILE_INFO), 1);
	if (!FileInfo)
	{
		free(Filename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	GetFileInfoInternal(SpFs, FileInfo, Filename);

	if (FileInfo->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		if (!Buffer)
		{
			free(Filename);
			free(FileInfo);
			return STATUS_SUCCESS;
		}
		unsigned long long FilenameIndex = 0;
		unsigned long long FilenameSTRIndex = 0;
		unsigned long long FileSize = 0;
		getfilenameindex(Filename, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		unsigned long long Index = gettablestrindex(Filename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		if (FileSize > *PSize)
		{
			free(Filename);
			free(FileInfo);
			return STATUS_BUFFER_TOO_SMALL;
		}
		char* buf = (char*)calloc(FileSize, 1);
		if (!buf)
		{
			free(Filename);
			free(FileInfo);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 0);
		memcpy(Buffer, buf, FileSize);
		*PSize = FileSize;
		
		free(Filename);
		free(buf);
		free(FileInfo);
		return STATUS_SUCCESS;
	}

	free(Filename);
	free(FileInfo);
	return STATUS_NOT_A_REPARSE_POINT;
}

static NTSTATUS GetReparsePointByName(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, BOOLEAN IsDirectory, PVOID Buffer, PSIZE_T PSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FileNameLen = wcslen(FileName);
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(Filename, FileName, FileNameLen * sizeof(wchar_t));
	ReplaceBSWFS(Filename);
	if (!NT_SUCCESS(FindDuplicate(SpFs, Filename)))
	{
		free(Filename);
		return GetReparsePoint(FileSystem, FileContext, FileName, Buffer, PSize);
	}

	free(Filename);
	return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long FileNameLen = wcslen(FileName);
	unsigned long winattrs = 0;
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, FileNameLen * sizeof(wchar_t));
	ReplaceBSWFS(Filename);

	if (NT_SUCCESS(FindDuplicate(SpFs, Filename)))
	{
		free(Filename);
		if (FspFileSystemFindReparsePoint(FileSystem, GetReparsePointByName, 0, FileName, PFileAttributes))
		{
			return STATUS_REPARSE;
		}
		return STATUS_OBJECT_NAME_NOT_FOUND;
	}

	if (PFileAttributes)
	{
		getfilenameindex(Filename, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
		ATTRtoattr(winattrs);
		*PFileAttributes = winattrs;
	}

	if (PSecurityDescriptorSize)
	{
		PWSTR SecurityName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
		if (!SecurityName)
		{
			free(Filename);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memcpy(SecurityName, Filename, FileNameLen * sizeof(wchar_t));
		RemoveFirst(SecurityName);
		PWSTR Suffix = NULL;
		RemoveStream(SecurityName, Suffix);
		PSECURITY_DESCRIPTOR S;
		FilenameIndex = 0;
		FilenameSTRIndex = 0;
		getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		unsigned long long Index = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		free(SecurityName);
		unsigned long long FileSize = 0;
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		if (*PSecurityDescriptorSize < FileSize)
		{
			*PSecurityDescriptorSize = FileSize;
			free(Filename);
			return STATUS_BUFFER_OVERFLOW;
		}
		char* buf = (char*)calloc(FileSize + 1, 1);
		readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 0);
		ConvertStringSecurityDescriptorToSecurityDescriptorA(buf, SDDL_REVISION_1, &S, (PULONG)PSecurityDescriptorSize);
		if (SecurityDescriptor)
		{
			memcpy(SecurityDescriptor, S, *PSecurityDescriptorSize);
		}
		free(buf);
		FspDeleteSecurityDescriptor(S, (NTSTATUS(*)())ConvertStringSecurityDescriptorToSecurityDescriptorA);
	}

	free(Filename);

	return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileContext;
	NTSTATUS Result = 0;
	unsigned long long FileNameLen = wcslen(FileName);
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, FileNameLen * sizeof(wchar_t));
	ReplaceBSWFS(Filename);

	Result = FindDuplicate(SpFs, Filename);
	if (!NT_SUCCESS(Result))
	{
		free(Filename);
		return Result;
	}

	FileContext = (SPFS_FILE_CONTEXT*)calloc(sizeof(*FileContext), 1);
	if (!FileContext)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memset(FileContext, 0, sizeof(*FileContext));

	FileContext->Path = Filename;
	*PFileContext = FileContext;

	PWSTR ParentDirectory = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!ParentDirectory)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR Suffix = 0;
	unsigned long gid = 0;
	unsigned long uid = 0;
	unsigned long winattrs = FileAttributes;
	unsigned long long ParentDirectoryIndex = 0;
	unsigned long long ParentDirectorySTRIndex = 0;

	attrtoATTR(winattrs);

	memcpy(ParentDirectory, Filename, FileNameLen * sizeof(wchar_t));
	GetParentName(ParentDirectory, Suffix);
	unsigned long long ParentIndex = gettablestrindex(ParentDirectory, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilenameindex(ParentDirectory, SpFs->Filenames, SpFs->FilenameCount, ParentDirectoryIndex, ParentDirectorySTRIndex);
	chgid(SpFs->FileInfo, SpFs->FilenameCount, ParentDirectoryIndex, gid, 0);
	chuid(SpFs->FileInfo, SpFs->FilenameCount, ParentDirectoryIndex, uid, 0);
	unsigned long long ParentDirectoryLen = wcslen(ParentDirectory);
	PWSTR SecurityParentName = (PWSTR)calloc(ParentDirectoryLen + 1, sizeof(wchar_t));
	if (!SecurityParentName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityParentName, ParentDirectory, ParentDirectoryLen * sizeof(wchar_t));
	RemoveFirst(SecurityParentName);
	free(ParentDirectory);

	createfile(Filename, gid, uid, 448 + (FileAttributes & FILE_ATTRIBUTE_DIRECTORY) * 16429, winattrs, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);

	if (std::wstring(Filename).find(L":") == std::string::npos)
	{
		PWSTR SecurityName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
		if (!SecurityName)
		{
			free(SecurityParentName);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memcpy(SecurityName, Filename, FileNameLen * sizeof(wchar_t));
		RemoveFirst(SecurityName);

		PULONG BufLen = (PULONG)calloc(sizeof(PULONG), 1);
		if (!BufLen)
		{
			free(SecurityParentName);
			free(SecurityName);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		LPSTR* Buf = (LPSTR*)calloc(1, sizeof(LPSTR*));
		if (!Buf)
		{
			free(SecurityParentName);
			free(SecurityName);
			free(BufLen);
			return STATUS_INSUFFICIENT_RESOURCES;
		}

		createfile(SecurityName, gid, uid, 448, 2048, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);
		unsigned long long SecurityIndex = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		unsigned long long SecurityFileIndex = 0;
		unsigned long long SecurityFileSTRIndex = 0;
		getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, SecurityFileIndex, SecurityFileSTRIndex);
		ConvertSecurityDescriptorToStringSecurityDescriptorA(SecurityDescriptor, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, Buf, BufLen);
		*BufLen = strlen(*Buf);
		unsigned long long FileSize = 0;
		if (std::string(*Buf).find("D:P") == std::string::npos)
		{
			unsigned long long SecurityParentIndex = gettablestrindex(SecurityParentName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
			unsigned long long SecurityParentDirectoryIndex = 0;
			unsigned long long SecurityParentDirectorySTRIndex = 0;
			getfilenameindex(SecurityParentName, SpFs->Filenames, SpFs->FilenameCount, SecurityParentDirectoryIndex, SecurityParentDirectorySTRIndex);
			getfilesize(SpFs->SectorSize, SecurityParentIndex, SpFs->TableStr, FileSize);
			if (*BufLen < FileSize)
			{
				LPSTR ALC = (LPSTR)realloc(*Buf, FileSize);
				if (!ALC)
				{
					free(SecurityParentName);
					free(SecurityName);
					free(BufLen);
					free(Buf);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				*Buf = ALC;
				ALC = NULL;
			}
			*BufLen = FileSize;
			readwritefile(SpFs->hDisk, SpFs->SectorSize, SecurityParentIndex, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, *Buf, SpFs->FileInfo, SecurityParentDirectoryIndex, 0);
		}
		if (trunfile(SpFs->hDisk, SpFs->SectorSize, SecurityIndex, SpFs->TableSize, SpFs->DiskSize, 0, *BufLen, SecurityFileIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, SecurityName, SpFs->Filenames, SpFs->FilenameCount))
		{
			unsigned long long Index = gettablestrindex(Filename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
			unsigned long long FileIndex = 0;
			unsigned long long FileSTRIndex = 0;
			getfilenameindex(Filename, SpFs->Filenames, SpFs->FilenameCount, FileIndex, FileSTRIndex);
			getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
			trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, FileIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, Filename, SpFs->Filenames, SpFs->FilenameCount);
			deletefile(Index, FileIndex, FileSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
			deletefile(SecurityIndex, SecurityFileIndex, SecurityFileSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
			free(SecurityParentName);
			free(SecurityName);
			free(BufLen);
			free(Buf);
			return STATUS_DISK_FULL;
		}
		readwritefile(SpFs->hDisk, SpFs->SectorSize, SecurityIndex, 0, *BufLen, SpFs->DiskSize, SpFs->TableStr, *Buf, SpFs->FileInfo, SecurityFileIndex, 1);
		free(SecurityName);
		free(BufLen);
		free(Buf);
	}

	free(SecurityParentName);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

	std::wstring Path = Filename;
	opened[Path]++;
	allocationsizes[Path] = AllocationSize;
	return GetFileInfoInternal(SpFs, FileInfo, Filename);
}

static NTSTATUS Open(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileContext = (SPFS_FILE_CONTEXT*)calloc(sizeof(*FileContext), 1);
	unsigned long long FileNameLen = wcslen(FileName);
	if (!FileContext)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memset(FileContext, 0, sizeof(*FileContext));
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, FileNameLen * sizeof(wchar_t));
	ReplaceBSWFS(Filename);

	FileContext->Path = Filename;
	*PFileContext = FileContext;

	std::wstring Path = Filename;
	opened[Path]++;
	allocationsizes[Path] = 0;
	allocationsizes.erase(Path);
	return GetFileInfoInternal(SpFs, FileInfo, Filename);
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long TempIndex = 0;
	unsigned long long TempFilenameIndex = 0;
	unsigned long long TempFilenameSTRIndex = 0;

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);

	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = max(FileNameLen, 0xff);
	PWSTR ALC = NULL;
	PWSTR TempFilename = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!TempFilename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	unsigned long long O = 0;
	PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameNoStream)
	{
		free(TempFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(FileNameNoStream, FileCtx->Path, FileNameLen * sizeof(wchar_t));
	PWSTR Suffix = NULL;
	RemoveStream(FileNameNoStream, Suffix);
	unsigned long long NoStreamFileNameIndex = 0;
	unsigned long long NoStreamFileNameSTRIndex = 0;
	getfilenameindex(FileNameNoStream, SpFs->Filenames, SpFs->FilenameCount, NoStreamFileNameIndex, NoStreamFileNameSTRIndex);
	std::wstring Path = TempFilename;
	unsigned long long FileSize = 0;

	for (unsigned long long i = 0; i < SpFs->FilenameCount + O; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(TempFilename, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(TempFilename);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				TempFilename = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameNoStream, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(TempFilename);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameNoStream = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			TempFilename[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		TempFilename[j] = 0;

		memcpy(FileNameNoStream, TempFilename, (j + 1) * sizeof(wchar_t));
		RemoveStream(FileNameNoStream, Suffix);
		if (!_wcsicmp(FileNameNoStream, FileCtx->Path) && wcslen(FileNameNoStream) == FileNameLen)
		{
			Path = TempFilename;
			if (Path.find(L":") != std::string::npos)
			{
				if (!opened[Path])
				{
					TempFilenameIndex = 0;
					TempFilenameSTRIndex = 0;
					getfilenameindex(TempFilename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
					TempIndex = gettablestrindex(TempFilename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
					getfilesize(SpFs->SectorSize, TempIndex, SpFs->TableStr, FileSize);
					trunfile(SpFs->hDisk, SpFs->SectorSize, TempIndex, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, TempFilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, TempFilename, SpFs->Filenames, SpFs->FilenameCount);
					deletefile(TempIndex, TempFilenameIndex, TempFilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
					Offset -= wcslen(TempFilename) + 1;
					O++;
				}
			}
		}
	}

	free(TempFilename);
	free(FileNameNoStream);

	if (ReplaceFileAttributes)
	{
		unsigned long winattrs = FileAttributes;
		attrtoATTR(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	}
	else
	{
		unsigned long winattrs = 0;
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
		ATTRtoattr(winattrs);
		winattrs |= FileAttributes | 32;
		attrtoATTR(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	}

	FILETIME ltime;
	GetSystemTimeAsFileTime(&ltime);
	LONGLONG pltime = ((PLARGE_INTEGER)&ltime)->QuadPart;
	double LTime = (double)(pltime - 116444736000000000) / 10000000;
	chtime(SpFs->FileInfo, NoStreamFileNameIndex, LTime, 1);
	chtime(SpFs->FileInfo, NoStreamFileNameIndex, LTime, 3);
	chtime(SpFs->FileInfo, NoStreamFileNameIndex, LTime, 5);

	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);
	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = max(FileNameLen, 0xff);
	PWSTR ALC = NULL;
	PWSTR Filename = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(Filename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameSuffix)
	{
		free(Filename);
		free(FileNameParent);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(Filename, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				Filename = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameParent, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameParent = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameSuffix, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameSuffix = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			Filename[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		Filename[j] = 0;

		memcpy(FileNameParent, Filename, (j + 1) * sizeof(wchar_t));
		GetParentName(FileNameParent, FileNameSuffix);
		if (!_wcsicmp(FileNameParent, FileCtx->Path) && wcslen(FileNameParent) == FileNameLen)
		{
			if (wcslen(FileNameSuffix))
			{
				free(Filename);
				free(FileNameParent);
				free(FileNameSuffix);
				return STATUS_DIRECTORY_NOT_EMPTY;
			}
		}
	}

	free(Filename);
	free(FileNameParent);
	free(FileNameSuffix);
	return STATUS_SUCCESS;
}

static VOID Cleanup(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long winattrs = 0;

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
	ATTRtoattr(winattrs);

	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	PWSTR NoStreamFileName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!NoStreamFileName)
	{
		return;
	}
	memcpy(NoStreamFileName, FileCtx->Path, FileNameLen * sizeof(wchar_t));
	PWSTR Suffix = NULL;
	RemoveStream(NoStreamFileName, Suffix);
	unsigned long long NoStreamFileNameIndex = 0;
	unsigned long long NoStreamFileNameSTRIndex = 0;
	getfilenameindex(NoStreamFileName, SpFs->Filenames, SpFs->FilenameCount, NoStreamFileNameIndex, NoStreamFileNameSTRIndex);
	free(NoStreamFileName);

	if (Flags & FspCleanupSetArchiveBit)
	{
		if (!(winattrs & FILE_ATTRIBUTE_DIRECTORY))
		{
			winattrs |= FILE_ATTRIBUTE_ARCHIVE;
			attrtoATTR(winattrs);
			chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
		}
	}

	FILETIME ltime;
	GetSystemTimeAsFileTime(&ltime);
	LONGLONG pltime = ((PLARGE_INTEGER)&ltime)->QuadPart;
	double LTime = (double)(pltime - 116444736000000000) / 10000000;

	if (Flags & FspCleanupSetLastAccessTime)
	{
		chtime(SpFs->FileInfo, NoStreamFileNameIndex, LTime, 1);
	}

	if (Flags & FspCleanupSetLastWriteTime || Flags & FspCleanupSetChangeTime)
	{
		chtime(SpFs->FileInfo, NoStreamFileNameIndex, LTime, 3);
	}

	if (Flags & FspCleanupDelete)
	{
		if (winattrs & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (!NT_SUCCESS(CanDelete(FileSystem, FileContext, FileCtx->Path)))
			{
				return;
			}
		}
		unsigned long long FileSize = 0;
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount);
		deletefile(Index, FilenameIndex, FilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
		if (std::wstring(FileCtx->Path).find(L":") != std::string::npos)
		{
			return;
		}
		Index = gettablestrindex(FileCtx->Path + 1, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		FilenameIndex = 0;
		FilenameSTRIndex = 0;
		getfilenameindex(FileCtx->Path + 1, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, FileCtx->Path + 1, SpFs->Filenames, SpFs->FilenameCount);
		deletefile(Index, FilenameIndex, FilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);

		unsigned long long TempIndex = 0;
		unsigned long long TempFilenameIndex = 0;
		unsigned long long TempFilenameSTRIndex = 0;
		unsigned long long Offset = 0;
		unsigned long long O = 0;
		unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = max(FileNameLen, 0xff);
		PWSTR ALC = NULL;
		PWSTR Filename = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
		if (!Filename)
		{
			return;
		}
		PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
		if (!FileNameNoStream)
		{
			free(Filename);
			return;
		}

		for (unsigned long long i = 0; i < SpFs->FilenameCount + O; i++)
		{
			unsigned long long j = 0;
			for (;; j++)
			{
				if (j > FileNameLenT - 3)
				{
					FileNameLenT += 0xff;
					ALC = (PWSTR)realloc(Filename, (FileNameLenT + 1) * sizeof(wchar_t));
					if (!ALC)
					{
						free(Filename);
						free(FileNameNoStream);
						return;
					}
					Filename = ALC;
					ALC = NULL;
					ALC = (PWSTR)realloc(FileNameNoStream, (FileNameLenT + 1) * sizeof(wchar_t));
					if (!ALC)
					{
						free(Filename);
						free(FileNameNoStream);
						return;
					}
					FileNameNoStream = ALC;
					ALC = NULL;
				}
				if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
				{
					Offset += j + 1;
					break;
				}
				Filename[j] = SpFs->Filenames[Offset + j] & 0xff;
			}
			Filename[j] = 0;

			memcpy(FileNameNoStream, Filename, (j + 1) * sizeof(wchar_t));
			RemoveStream(FileNameNoStream, Suffix);
			if (!_wcsicmp(FileNameNoStream, FileCtx->Path) && wcslen(FileNameNoStream) == FileNameLen)
			{
				if (std::wstring(Filename).find(L":") != std::string::npos)
				{
					TempFilenameIndex = 0;
					TempFilenameSTRIndex = 0;
					getfilenameindex(Filename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
					TempIndex = gettablestrindex(Filename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
					getfilesize(SpFs->SectorSize, TempIndex, SpFs->TableStr, FileSize);
					trunfile(SpFs->hDisk, SpFs->SectorSize, TempIndex, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, TempFilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, Filename, SpFs->Filenames, SpFs->FilenameCount);
					deletefile(TempIndex, TempFilenameIndex, TempFilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
					Offset -= wcslen(Filename) + 1;
					O++;
				}
			}
		}

		free(Filename);
		free(FileNameNoStream);

		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);
	}

	return;
}

static VOID Close(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext)
{
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	std::wstring Path = FileCtx->Path;
	allocationsizes[Path] = 0;
	allocationsizes.erase(Path);
	if (opened[Path])
	{
		if (opened[Path] != 1)
		{
			opened[Path]--;
		}
		else
		{
			opened.erase(Path);
		}
	}
	free(FileCtx->Path);
	free(FileCtx);
	return;
}

static NTSTATUS Read(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, PULONG PBytesTransffered)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FileSize = 0;
	unsigned long long FileNameIndex = 0;
	unsigned long long FileNameSTRIndex = 0;

	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	if (Offset >= FileSize)
	{
		return STATUS_END_OF_FILE;
	}
	Length = min(Length, FileSize - Offset);
	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FileNameIndex, FileNameSTRIndex);
	char* Buf = (char*)Buffer;
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, Offset, Length, SpFs->DiskSize, SpFs->TableStr, Buf, SpFs->FileInfo, FileNameIndex, 0);
	*PBytesTransffered = Length;

	return STATUS_SUCCESS;
}

static NTSTATUS Write(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	NTSTATUS Result = 0;

	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FileSize = 0;
	unsigned long long FileNameIndex = 0;
	unsigned long long FileNameSTRIndex = 0;

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FileNameIndex, FileNameSTRIndex);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	if (WriteToEndOfFile)
	{
		Offset = FileSize;
	}
	if (Offset + Length > FileSize)
	{
		if (trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, Offset + Length, FileNameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount))
		{
			return STATUS_DISK_FULL;
		}
		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);
	}

	char* Buf = (char*)Buffer;
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, Offset, Length, SpFs->DiskSize, SpFs->TableStr, Buf, SpFs->FileInfo, FileNameIndex, 1);
	*PBytesTransferred = Length;
	Result = GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);

	return Result;
}

static NTSTATUS Flush(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT32 FileAttributes, UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long winattrs = FileAttributes;

	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	PWSTR NoStreamFileName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!NoStreamFileName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(NoStreamFileName, FileCtx->Path, FileNameLen * sizeof(wchar_t));
	PWSTR Suffix = NULL;
	RemoveStream(NoStreamFileName, Suffix);
	unsigned long long NoStreamFileNameIndex = 0;
	unsigned long long NoStreamFileNameSTRIndex = 0;
	getfilenameindex(NoStreamFileName, SpFs->Filenames, SpFs->FilenameCount, NoStreamFileNameIndex, NoStreamFileNameSTRIndex);
	free(NoStreamFileName);

	if (winattrs != INVALID_FILE_ATTRIBUTES)
	{
		attrtoATTR(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	}

	if (LastAccessTime)
	{
		double ATime = (LastAccessTime - static_cast<double>(116444736000000000)) / 10000000;
		chtime(SpFs->FileInfo, NoStreamFileNameIndex, ATime, 1);
	}

	if (LastWriteTime || ChangeTime)
	{
		LastWriteTime = max(LastWriteTime, ChangeTime);
		double WTime = (LastWriteTime - static_cast<double>(116444736000000000)) / 10000000;
		chtime(SpFs->FileInfo, NoStreamFileNameIndex, WTime, 3);
	}

	if (CreationTime)
	{
		double CTime = (CreationTime - static_cast<double>(116444736000000000)) / 10000000;
		chtime(SpFs->FileInfo, NoStreamFileNameIndex, CTime, 5);
	}

	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	if (!SetAllocationSize)
	{
		unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		unsigned long long FileSize = 0;
		unsigned long long FileNameIndex = 0;
		unsigned long long FileNameSTRIndex = 0;

		getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FileNameIndex, FileNameSTRIndex);
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		if (trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, NewSize, FileNameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount))
		{
			return STATUS_DISK_FULL;
		}
		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);
	}

	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS Rename(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long TempIndex = 0;
	unsigned long long TempFilenameIndex = 0;
	unsigned long long TempFilenameSTRIndex = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	unsigned long long NewFileNameLen = wcslen(NewFileName);
	NTSTATUS Result = 0;

	PWSTR NewFilename = (PWSTR)calloc(NewFileNameLen + 1, sizeof(wchar_t));
	if (!NewFilename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(NewFilename, NewFileName, NewFileNameLen * sizeof(wchar_t));
	ReplaceBSWFS(NewFilename);

	if (!(FileNameLen == NewFileNameLen && !_wcsicmp(FileCtx->Path, NewFilename)))
	{
		Result = FindDuplicate(SpFs, NewFilename);
		if (!NT_SUCCESS(Result))
		{
			if (!ReplaceIfExists)
			{
				free(NewFilename);
				return Result;
			}
			else
			{
				FSP_FSCTL_FILE_INFO* FileInfo = (FSP_FSCTL_FILE_INFO*)calloc(1, sizeof(FSP_FSCTL_FILE_INFO));
				GetFileInfoInternal(SpFs, FileInfo, NewFilename);
				if (!(FileInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					free(FileInfo);
					SPFS_FILE_CONTEXT* NewFileCtx = (SPFS_FILE_CONTEXT*)calloc(1, sizeof(SPFS_FILE_CONTEXT));
					if (!NewFileCtx)
					{
						free(NewFilename);
						return STATUS_INSUFFICIENT_RESOURCES;
					}
					NewFileCtx->Path = NewFilename;
					Cleanup(FileSystem, NewFileCtx, NewFilename, FspCleanupDelete);
					free(NewFileCtx);
					Result = FindDuplicate(SpFs, NewFilename);
					if (!NT_SUCCESS(Result))
					{
						free(NewFilename);
						return Result;
					}
				}
				else
				{
					free(FileInfo);
					return STATUS_ACCESS_DENIED;
				}
			}
		}
	}

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	renamefile(FileCtx->Path, NewFilename, FilenameSTRIndex, SpFs->Filenames);

	PWSTR Suffix = NULL;
	PWSTR SecurityName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!SecurityName)
	{
		free(NewFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileCtx->Path, FileNameLen * sizeof(wchar_t));
	RemoveFirst(SecurityName);
	RemoveStream(SecurityName, Suffix);
	FilenameIndex = 0;
	FilenameSTRIndex = 0;
	getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	PWSTR NewSecurityName = (PWSTR)calloc(NewFileNameLen + 1, sizeof(wchar_t));
	if (!NewSecurityName)
	{
		free(NewFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(NewSecurityName, NewFilename, NewFileNameLen * sizeof(wchar_t));
	RemoveFirst(NewSecurityName);
	RemoveStream(NewSecurityName, Suffix);
	renamefile(SecurityName, NewSecurityName, FilenameSTRIndex, SpFs->Filenames);

	unsigned long long Offset = 0;
	unsigned long long FileNameLenT = max(FileNameLen, 0xff);
	PWSTR ALC = NULL;
	PWSTR TempFilename = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!TempFilename)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		free(TempFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameSuffix)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		free(TempFilename);
		free(FileNameParent);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(TempFilename, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(NewFilename);
					free(SecurityName);
					free(NewSecurityName);
					free(TempFilename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				TempFilename = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameParent, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(NewFilename);
					free(SecurityName);
					free(NewSecurityName);
					free(TempFilename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameParent = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameSuffix, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(NewFilename);
					free(SecurityName);
					free(NewSecurityName);
					free(TempFilename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameSuffix = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			TempFilename[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		TempFilename[j] = 0;

		memcpy(FileNameParent, TempFilename, (j + 1) * sizeof(wchar_t));
		GetParentName(FileNameParent, FileNameSuffix);
		if (!_wcsicmp(FileNameParent, FileCtx->Path) && (wcslen(FileNameParent) == FileNameLen || FileNameParent[FileNameLen] == *L"/"))
		{
			if (wcslen(FileNameSuffix))
			{
				TempFilenameIndex = 0;
				TempFilenameSTRIndex = 0;
				getfilenameindex(TempFilename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
				renamefile(TempFilename, (PWSTR)(std::wstring(FileNameParent).replace(0, FileNameLen, NewFilename) + L"/" + FileNameSuffix).c_str(), TempFilenameSTRIndex, SpFs->Filenames);
				if (std::wstring(TempFilename).find(L":") == std::string::npos)
				{
					TempFilenameIndex = 0;
					TempFilenameSTRIndex = 0;
					getfilenameindex(TempFilename + 1, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
					renamefile(TempFilename + 1, (PWSTR)(std::wstring(FileNameParent).replace(0, FileNameLen, NewFilename) + L"/" + FileNameSuffix).c_str() + 1, TempFilenameSTRIndex, SpFs->Filenames);
				}
				Offset += wcslen(std::wstring(FileNameParent).replace(0, FileNameLen, NewFilename).c_str());
				Offset -= FileNameLen;
			}
		}
	}

	Offset = 0;
	unsigned long long O = 0;
	PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameNoStream)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		free(TempFilename);
		free(FileNameParent);
		free(FileNameSuffix);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	unsigned long long FileSize = 0;

	for (unsigned long long i = 0; i < SpFs->FilenameCount + O; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(TempFilename, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(NewFilename);
					free(SecurityName);
					free(NewSecurityName);
					free(TempFilename);
					free(FileNameParent);
					free(FileNameSuffix);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				TempFilename = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameNoStream, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(NewFilename);
					free(SecurityName);
					free(NewSecurityName);
					free(TempFilename);
					free(FileNameParent);
					free(FileNameSuffix);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameNoStream = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			TempFilename[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		TempFilename[j] = 0;

		memcpy(FileNameNoStream, TempFilename, (j + 1) * sizeof(wchar_t));
		RemoveStream(FileNameNoStream, Suffix);
		if (!_wcsicmp(FileNameNoStream, FileCtx->Path) && wcslen(FileNameNoStream) == FileNameLen)
		{
			if (std::wstring(TempFilename).find(L":") != std::string::npos)
			{
				TempFilenameIndex = 0;
				TempFilenameSTRIndex = 0;
				getfilenameindex(TempFilename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
				TempIndex = gettablestrindex(TempFilename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
				getfilesize(SpFs->SectorSize, TempIndex, SpFs->TableStr, FileSize);
				trunfile(SpFs->hDisk, SpFs->SectorSize, TempIndex, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, TempFilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, TempFilename, SpFs->Filenames, SpFs->FilenameCount);
				deletefile(TempIndex, TempFilenameIndex, TempFilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
				Offset -= wcslen(TempFilename) + 1;
				O++;
			}
		}
	}

	free(TempFilename);
	free(FileNameParent);
	free(FileNameSuffix);
	free(FileNameNoStream);

	ALC = (PWSTR)realloc(FileCtx->Path, (NewFileNameLen + 1) * sizeof(wchar_t));
	if (!ALC)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	FileCtx->Path = ALC;
	ALC = NULL;
	FileCtx->Path[NewFileNameLen] = 0;
	memcpy(FileCtx->Path, NewFilename, NewFileNameLen * sizeof(wchar_t));
	free(NewFilename);
	free(SecurityName);
	free(NewSecurityName);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

	return Result;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	PWSTR Suffix = NULL;
	PWSTR SecurityName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!SecurityName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileCtx->Path, FileNameLen * sizeof(wchar_t));

	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;

	RemoveFirst(SecurityName);
	RemoveStream(SecurityName, Suffix);
	PSECURITY_DESCRIPTOR S;
	getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	unsigned long long Index = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	free(SecurityName);
	unsigned long long FileSize = 0;

	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	if (*PSecurityDescriptorSize < FileSize)
	{
		*PSecurityDescriptorSize = FileSize;
		return STATUS_BUFFER_OVERFLOW;
	}
	char* buf = (char*)calloc(FileSize + 1, 1);
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, (char*&)buf, SpFs->FileInfo, FilenameIndex, 0);
	ConvertStringSecurityDescriptorToSecurityDescriptorA(buf, SDDL_REVISION_1, &S, (PULONG)PSecurityDescriptorSize);
	memcpy(SecurityDescriptor, S, *PSecurityDescriptorSize);
	FspDeleteSecurityDescriptor(S, (NTSTATUS(*)())ConvertStringSecurityDescriptorToSecurityDescriptorA);
	free(buf);

	return STATUS_SUCCESS;
}

static NTSTATUS SetSecurity(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	PWSTR Suffix = NULL;
	PWSTR SecurityName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!SecurityName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileCtx->Path, FileNameLen * sizeof(wchar_t));

	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;

	RemoveFirst(SecurityName);
	RemoveStream(SecurityName, Suffix);
	PULONG PSecurityDescriptorSize = (PULONG)calloc(sizeof(PULONG), 1);
	if (!PSecurityDescriptorSize)
	{
		free(SecurityName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PSECURITY_DESCRIPTOR S;
	getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	unsigned long long Index = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FileSize = 0;

	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	char* buf = (char*)calloc(FileSize + 1, 1);
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 0);
	ConvertStringSecurityDescriptorToSecurityDescriptorA(buf, SDDL_REVISION_1, &S, (PULONG)PSecurityDescriptorSize);
	free(buf);

	PSECURITY_DESCRIPTOR NewSecurityDescriptor;
	NTSTATUS Result;

	if (SecurityInformation & 1)
	{
		Result = SetSecurityDescriptor(S, 1, ModificationDescriptor, &NewSecurityDescriptor);
		if (NT_SUCCESS(Result))
		{
			Result = FspSetSecurityDescriptor(NewSecurityDescriptor, SecurityInformation - 1, ModificationDescriptor, &NewSecurityDescriptor);
		}
	}
	else
	{
		Result = FspSetSecurityDescriptor(S, SecurityInformation, ModificationDescriptor, &NewSecurityDescriptor);
	}
	if (!NT_SUCCESS(Result))
	{
		free(PSecurityDescriptorSize);
		free(SecurityName);
		return Result;
	}
	if (SecurityInformation & 1)
	{
		FspDeleteSecurityDescriptor(S, (NTSTATUS(*)())SetSecurityDescriptor);
	}
	else
	{
		FspDeleteSecurityDescriptor(S, (NTSTATUS(*)())FspSetSecurityDescriptor);
	}

	LPSTR* Buf = (LPSTR*)calloc(1, sizeof(LPSTR*));
	if (!Buf)
	{
		free(PSecurityDescriptorSize);
		free(SecurityName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	ConvertSecurityDescriptorToStringSecurityDescriptorA(NewSecurityDescriptor, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, Buf, (PULONG)PSecurityDescriptorSize);
	FspDeleteSecurityDescriptor(NewSecurityDescriptor, (NTSTATUS(*)())FspSetSecurityDescriptor);

	*PSecurityDescriptorSize = strlen(*Buf);
	if (trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, *PSecurityDescriptorSize, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, SecurityName, SpFs->Filenames, SpFs->FilenameCount))
	{
		free(PSecurityDescriptorSize);
		free(SecurityName);
		free(Buf);
		return STATUS_DISK_FULL;
	}
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, *PSecurityDescriptorSize, SpFs->DiskSize, SpFs->TableStr, *Buf, SpFs->FileInfo, FilenameIndex, 1);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);
	free(PSecurityDescriptorSize);
	free(SecurityName);
	free(Buf);

	return Result;
}

static BOOLEAN AddDirInfo(SPFS* SpFs, PWSTR Name, PWSTR FileName, PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
	unsigned long long FileNameLen = wcslen(FileName);
	FSP_FSCTL_DIR_INFO* DirInfo = (FSP_FSCTL_DIR_INFO*)calloc(sizeof(FSP_FSCTL_DIR_INFO) + FileNameLen * sizeof(wchar_t), 1);
	if (!DirInfo)
	{
		return FALSE;
	}

	memset(DirInfo->Padding, 0, sizeof(DirInfo->Padding));
	DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + FileNameLen * sizeof(wchar_t));
	GetFileInfoInternal(SpFs, &DirInfo->FileInfo, Name);
	memcpy(DirInfo->FileNameBuf, FileName, DirInfo->Size - sizeof(FSP_FSCTL_DIR_INFO));
	BOOLEAN Result = FspFileSystemAddDirInfo(DirInfo, Buffer, Length, PBytesTransferred);

	free(DirInfo);
	return Result;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR Pattern, PWSTR Marker, PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	PWSTR Suffix = 0;
	PWSTR ParentDirectoryName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!ParentDirectoryName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(ParentDirectoryName, FileCtx->Path, FileNameLen * sizeof(wchar_t));
	GetParentName(ParentDirectoryName, Suffix);

	if (FileCtx->Path[1] != L'\0')
	{
		if (!Marker)
		{
			if (!AddDirInfo(SpFs, FileCtx->Path, (PWSTR)L".", Buffer, BufferLength, PBytesTransferred))
			{
				free(ParentDirectoryName);
				return STATUS_SUCCESS;
			}
		}

		if (!Marker || (Marker[0] == L'.' && Marker[1] == L'\0'))
		{
			if (!AddDirInfo(SpFs, ParentDirectoryName, (PWSTR)L"..", Buffer, BufferLength, PBytesTransferred))
			{
				free(ParentDirectoryName);
				return STATUS_SUCCESS;
			}

			free(ParentDirectoryName);
			Marker = 0;
		}
	}
	else
	{
		free(ParentDirectoryName);
	}

	unsigned long long Offset = 0;
	unsigned long long FileNameLenT = max(FileNameLen, 0xff);
	PWSTR ALC = NULL;
	PWSTR FileName = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(FileName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileNameSuffix)
	{
		free(FileName);
		free(FileNameParent);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	unsigned HitMarker = !Marker;
	unsigned long long MarkerLen = 0;
	if (Marker)
	{
		MarkerLen = wcslen(Marker);
	}
	unsigned long long FileNameSuffixLen = 0;
	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(FileName, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(FileName);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileName = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameParent, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(FileName);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameParent = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameSuffix, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(FileName);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameSuffix = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			FileName[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		FileName[j] = 0;

		memcpy(FileNameParent, FileName, (j + 1) * sizeof(wchar_t));
		GetParentName(FileNameParent, FileNameSuffix);
		if (!_wcsicmp(FileNameParent, FileCtx->Path) && wcslen(FileNameParent) == FileNameLen)
		{
			FileNameSuffixLen = wcslen(FileNameSuffix);
			if (FileNameSuffixLen)
			{
				if (std::wstring(FileNameSuffix).find(L":") == std::string::npos)
				{
					if (!HitMarker)
					{
						if (!_wcsicmp(FileNameSuffix, Marker) && FileNameSuffixLen == MarkerLen)
						{
							HitMarker = 1;
						}
					}
					else
					{
						if (!AddDirInfo(SpFs, FileName, FileNameSuffix, Buffer, BufferLength, PBytesTransferred))
						{
							free(FileName);
							free(FileNameParent);
							free(FileNameSuffix);
							return STATUS_SUCCESS;
						}
					}
				}
			}
		}
	}

	FspFileSystemAddDirInfo(0, Buffer, BufferLength, PBytesTransferred);

	free(FileName);
	free(FileNameParent);
	free(FileNameSuffix);

	return STATUS_SUCCESS;
}

static NTSTATUS ResolveReparsePoints(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 ReparsePointIndex, BOOLEAN ResolveLastPathComponent, PIO_STATUS_BLOCK PIoStatus, PVOID Buffer, PSIZE_T PSize)
{
	return FspFileSystemResolveReparsePoints(FileSystem, GetReparsePointByName, 0, FileName, ReparsePointIndex, ResolveLastPathComponent, PIoStatus, Buffer, PSize);
}

static NTSTATUS SetReparsePoint(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PVOID Buffer, SIZE_T Size)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	FSP_FSCTL_FILE_INFO* FileInfo = (FSP_FSCTL_FILE_INFO*)calloc(sizeof(FSP_FSCTL_FILE_INFO), 1);
	if (!FileInfo)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);

	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long FileSize = 0;
	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, Size, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount);
	char* buf = (char*)calloc(Size, 1);
	if (!buf)
	{
		free(FileInfo);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(buf, Buffer, Size);
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, Size, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 1);
	unsigned long winattrs = FileInfo->FileAttributes | FILE_ATTRIBUTE_REPARSE_POINT;
	attrtoATTR(winattrs);
	chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

	free(buf);
	free(FileInfo);
	return STATUS_SUCCESS;
}

static NTSTATUS DeleteReparsePoint(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PVOID Buffer, SIZE_T Size)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	FSP_FSCTL_FILE_INFO* FileInfo = (FSP_FSCTL_FILE_INFO*)calloc(sizeof(FSP_FSCTL_FILE_INFO), 1);
	if (!FileInfo)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);

	if (FileInfo->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		unsigned long long FilenameIndex = 0;
		unsigned long long FilenameSTRIndex = 0;
		unsigned long long FileSize = 0;
		getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, 0, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount);
		unsigned long winattrs = FileInfo->FileAttributes & ~FILE_ATTRIBUTE_REPARSE_POINT;
		attrtoATTR(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

		free(FileInfo);
		return STATUS_SUCCESS;
	}

	free(FileInfo);
	return STATUS_NOT_A_REPARSE_POINT;
}

static BOOLEAN AddStreamInfo(SPFS* SpFs, PWSTR Name, PWSTR FileName, PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
	unsigned long long FileNameLen = 0;
	if (FileName)
	{
		FileNameLen = wcslen(FileName);
	}
	FSP_FSCTL_STREAM_INFO* StreamInfo = (FSP_FSCTL_STREAM_INFO*)calloc(sizeof(FSP_FSCTL_STREAM_INFO) + FileNameLen * sizeof(wchar_t), 1);
	if (!StreamInfo)
	{
		return FALSE;
	}
	FSP_FSCTL_FILE_INFO* FileInfo = (FSP_FSCTL_FILE_INFO*)calloc(sizeof(FSP_FSCTL_FILE_INFO), 1);
	if (!FileInfo)
	{
		free(StreamInfo);
		return FALSE;
	}

	GetFileInfoInternal(SpFs, FileInfo, Name);

	StreamInfo->Size = (UINT16)(sizeof(FSP_FSCTL_STREAM_INFO) + FileNameLen * sizeof(wchar_t));
	StreamInfo->StreamSize = FileInfo->FileSize;
	StreamInfo->StreamAllocationSize = FileInfo->AllocationSize;
	memcpy(StreamInfo->StreamNameBuf, FileName, StreamInfo->Size - sizeof(FSP_FSCTL_STREAM_INFO));
	BOOLEAN Result = FspFileSystemAddStreamInfo(StreamInfo, Buffer, Length, PBytesTransferred);

	free(FileInfo);
	free(StreamInfo);
	return Result;
}

static NTSTATUS GetStreamInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long FileNameLen = wcslen(FileCtx->Path);
	PWSTR Suffix = NULL;
	PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileNameNoStream)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	FSP_FSCTL_FILE_INFO* FileInfo = (FSP_FSCTL_FILE_INFO*)calloc(sizeof(FSP_FSCTL_FILE_INFO), 1);
	if (!FileInfo)
	{
		free(FileNameNoStream);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(FileNameNoStream, FileCtx->Path, FileNameLen * sizeof(wchar_t));
	RemoveStream(FileNameNoStream, Suffix);
	GetFileInfoInternal(SpFs, FileInfo, FileNameNoStream);
	FileNameLen = wcslen(FileNameNoStream);

	if (!(FileInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		if (!AddStreamInfo(SpFs, FileNameNoStream, NULL, Buffer, Length, PBytesTransferred))
		{
			free(FileNameNoStream);
			free(FileInfo);
			return STATUS_SUCCESS;
		}
	}

	free(FileInfo);

	unsigned long long Offset = 0;
	unsigned long long FileNameLenT = max(FileNameLen, 0xff);
	PWSTR ALC = NULL;
	PWSTR FileName = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!FileName)
	{
		free(FileNameNoStream);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR Filename = (PWSTR)calloc(FileNameLenT + 1, sizeof(wchar_t));
	if (!Filename)
	{
		free(FileName);
		free(FileNameNoStream);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if (j > FileNameLenT - 3)
			{
				FileNameLenT += 0xff;
				ALC = (PWSTR)realloc(FileName, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileName);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileName = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(Filename, (FileNameLenT + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileName);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				Filename = ALC;
				ALC = NULL;
			}
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			FileName[j] = SpFs->Filenames[Offset + j] & 0xff;
		}
		FileName[j] = 0;

		memcpy(Filename, FileName, (j + 1) * sizeof(wchar_t));
		RemoveStream(FileName, Suffix);
		if (!_wcsicmp(FileName, FileNameNoStream) && wcslen(FileName) == FileNameLen)
		{
			if (std::wstring(Filename).find(L":") != std::wstring::npos)
			{
				if (!AddStreamInfo(SpFs, Filename, Suffix, Buffer, Length, PBytesTransferred))
				{
					free(Filename);
					free(FileName);
					free(FileNameNoStream);
					return STATUS_SUCCESS;
				}
			}
		}
	}

	FspFileSystemAddStreamInfo(0, Buffer, Length, PBytesTransferred);

	free(Filename);
	free(FileName);
	free(FileNameNoStream);
	return STATUS_SUCCESS;
}

static NTSTATUS GetDirInfoByName(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, FSP_FSCTL_DIR_INFO* DirInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	unsigned long long FileContextLen = wcslen(FileCtx->Path);
	unsigned long long FileNameLen = wcslen(FileName);

	PWSTR Filename = (PWSTR)calloc(FileContextLen + 1 + FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileCtx->Path, FileContextLen * sizeof(wchar_t));
	Filename[FileContextLen] = L'\\';
	memcpy(Filename + FileContextLen + 1, FileName, FileNameLen * sizeof(wchar_t));

	if (!NT_SUCCESS(FindDuplicate(SpFs, Filename)))
	{
		DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + FileNameLen * sizeof(wchar_t));
		GetFileInfoInternal(SpFs, &DirInfo->FileInfo, Filename);
		memcpy(DirInfo->FileNameBuf, FileName, DirInfo->Size - sizeof(FSP_FSCTL_DIR_INFO));

		free(Filename);
		return STATUS_SUCCESS;
	}

	free(Filename);
	return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS Control(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT32 ControlCode, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength, PULONG PBytesTransferred)
{
	if (CTL_CODE(0x8000 + 'M', 'R', METHOD_BUFFERED, FILE_ANY_ACCESS) == ControlCode)
	{
		if (OutputBufferLength != InputBufferLength)
		{
			return STATUS_INVALID_PARAMETER;
		}

		for (PUINT8 P = (PUINT8)InputBuffer, Q = (PUINT8)OutputBuffer, EndP = P + InputBufferLength; EndP > P; P++, Q++)
		{
			if (('A' <= *P && *P <= 'M') || ('a' <= *P && *P <= 'm'))
			{
				*Q = *P + 13;
			}
			else
				if (('N' <= *P && *P <= 'Z') || ('n' <= *P && *P <= 'z'))
				{
					*Q = *P - 13;
				}
				else
				{
					*Q = *P;
				}
		}

		*PBytesTransferred = InputBufferLength;
		return STATUS_SUCCESS;
	}

	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS GetEa(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PFILE_FULL_EA_INFORMATION Ea, ULONG EaLength, PULONG PBytesTransferred)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS SetEa(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PFILE_FULL_EA_INFORMATION Ea, ULONG EaLength, FSP_FSCTL_FILE_INFO* FileInfo)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static VOID DispatcherStopped(FSP_FILE_SYSTEM* FileSystem, BOOLEAN Normally)
{
	return;
}

static FSP_FILE_SYSTEM_INTERFACE SpFsInterface =
{
	GetVolumeInfo,
	SetVolumeLabel_,
	GetSecurityByName,
	Create,
	Open,
	Overwrite,
	Cleanup,
	Close,
	Read,
	Write,
	Flush,
	GetFileInfo,
	SetBasicInfo,
	SetFileSize,
	CanDelete,
	Rename,
	GetSecurity,
	SetSecurity,
	ReadDirectory,
	ResolveReparsePoints,
	GetReparsePoint,
	SetReparsePoint,
	DeleteReparsePoint,
	GetStreamInfo,
	GetDirInfoByName,
	Control,
	0,
	0,
	0,
	GetEa,
	SetEa,
	0,
	DispatcherStopped,
};

static VOID SpFsDelete(SPFS* SpFs)
{
	unsigned long long index = 0;
	unsigned long long filenameindex = 0;
	unsigned long long filenamestrindex = 0;
	getfilenameindex(PWSTR(L"?"), SpFs->Filenames, SpFs->FilenameCount, filenameindex, filenamestrindex);
	index = gettablestrindex(PWSTR(L"?"), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	deletefile(index, filenameindex, filenamestrindex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

	if (SpFs->FileSystem)
	{
		FspFileSystemDelete(SpFs->FileSystem);
	}

	if (SpFs->MountPoint)
	{
		free(SpFs->MountPoint);
	}

	if (SpFs->hDisk)
	{
		CloseHandle(SpFs->hDisk);
	}

	if (SpFs->Table)
	{
		free(SpFs->Table);
	}

	if (SpFs->TableStr)
	{
		free(SpFs->TableStr);
	}

	if (SpFs->Filenames)
	{
		free(SpFs->Filenames);
	}

	if (SpFs->FileInfo)
	{
		free(SpFs->FileInfo);
	}

	free(SpFs);
}

static NTSTATUS SpFsCreate(PWSTR Path, PWSTR MountPoint, UINT32 SectorSize, UINT32 DebugFlags, SPFS** PSpFs)
{
	FSP_FSCTL_VOLUME_PARAMS VolumeParams;
	SPFS* SpFs = 0;
	NTSTATUS Result = 0;

	*PSpFs = 0;

	// From now on we must goto exit on failure.
	
	_tzset();
	unsigned long sectorsize = 512;

	//std::cout << "Opening disk: " << argv[1] << std::endl;
	HANDLE hDisk = CreateFile(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hDisk == INVALID_HANDLE_VALUE)
	{
		std::cout << "Opening Error: " << GetLastError() << std::endl;
		return STATUS_UNSUCCESSFUL;
	}
	//std::cout << "Disk opened successfully" << std::endl;

	if (SectorSize)
	{
		sectorsize = SectorSize;
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
			return STATUS_UNSUCCESSFUL;
		}
		//std::cout << "Formatted disk with sectorsize: " << unsigned(pow(2, 9 + i)) << ", 2**(9+" << i << ")" << std::endl;
	}

	_LARGE_INTEGER disksize = { 0 };
	SetFilePointerEx(hDisk, disksize, &disksize, 2);
	if (!disksize.QuadPart)
	{
		DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &disksize, sizeof(disksize), NULL, NULL);
	}
	//std::cout << "Disk size: " << disksize.QuadPart << std::endl;

	_LARGE_INTEGER seek = { 0 };
	SetFilePointerEx(hDisk, seek, &seek, 0);
	char bytes[512] = { 0 };
	DWORD r;
	if (!ReadFile(hDisk, bytes, 512, &r, NULL))
	{
		std::cout << "Reading Error: " << GetLastError() << std::endl;
		return STATUS_UNSUCCESSFUL;
	}
	if (r != 512)
	{
		std::cout << "Reading Error: " << GetLastError() << std::endl;
		return STATUS_UNSUCCESSFUL;
	}
	sectorsize = pow(2, 9 + bytes[0]);
	//std::cout << "Read disk with sectorsize: " << sectorsize << std::endl;

	unsigned long tablesize = 1 + bytes[4] + (bytes[3] << 8) + (bytes[2] << 16) + (bytes[1] << 24);
	unsigned long long extratablesize = (static_cast<unsigned long long>(tablesize) * sectorsize) - 512;
	char* ttable = (char*)calloc(extratablesize, 1);
	if (!ReadFile(hDisk, ttable, extratablesize, &r, NULL))
	{
		std::cout << "Reading table Error: " << GetLastError() << std::endl;
		return STATUS_UNSUCCESSFUL;
	}
	if (r != extratablesize)
	{
		std::cout << "Reading table Error: " << GetLastError() << std::endl;
		return STATUS_UNSUCCESSFUL;
	}
	//std::cout << "Read table with size: " << tablesize << std::endl;

	char* table = (char*)calloc(512 + static_cast<size_t>(extratablesize), 1);
	memcpy(table, bytes, 512);
	memcpy(table + 512, ttable, extratablesize);
	free(ttable);

	std::unordered_map<unsigned, unsigned> Emap = {};
	std::unordered_map<unsigned, unsigned> Dmap = {};
	unsigned p = 0;
	unsigned c;
	for (unsigned i = 0; i < 15; i++)
	{
		for (unsigned o = 0; o < 15; o++)
		{
			c = charmap[i] << 8 | charmap[o];
			Emap[c] = p;
			Dmap[p] = c;
			p++;
		}
	}

	handmaps(Emap, Dmap, filenameindexlist);

	unsigned long long pos = 0;
	while (((unsigned)table[pos] & 0xff) != 255)
	{
		pos++;
	}
	//std::cout << "Found end of table at: " << pos << std::endl;
	//std::cout << "Table size: " << pos - 5 << std::endl;
	char* tablestr = (char*)calloc(pos - 5 + 1, 1);
	memcpy(tablestr, table + 5, pos - 5);
	decode(tablestr, pos - 5);
	//std::cout << "Decoded table: " << std::string(str, (pos - 5) * 2) << std::endl;

	//simp(charmap, tablestr);
	//encode(str, (pos - 5) * 2);
	//std::cout << "Encoded table: ";
	//for (unsigned long long i = 0; i < pos - 5; i++)
	//{
	//	std::cout << ((unsigned)str[i] & 0xff) << " ";
	//}
	//std::cout << std::endl;

	unsigned long long filenamepos = pos;
	while (((unsigned)table[filenamepos] & 0xff) != 254)
	{
		filenamepos++;
	}
	//std::cout << "Found end of filenames at: " << filenamepos << std::endl;
	//std::cout << "Filenames size: " << filenamepos - pos << std::endl;
	char* filenames = (char*)calloc(filenamepos - pos + 1, 1);
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
	unsigned long long index = 0;
	unsigned long long filenameindex = 0;
	unsigned long long filenamestrindex = 0;
	unsigned long long filesize = 0;
	unsigned long winattrs = 0;

	// Need to init SpaceFS ^

	unsigned o = 9;
	SpFs = (SPFS*)malloc(sizeof(*SpFs));
	if (!SpFs)
	{
		Result = STATUS_INSUFFICIENT_RESOURCES;
		goto exit;
	}
	memset(SpFs, 0, sizeof(*SpFs));

	// Allocate SpFs ^

	SpFs->hDisk = hDisk;
	SpFs->SectorSize = sectorsize;
	SpFs->DiskSize = disksize.QuadPart;
	SpFs->TableSize = tablesize;
	SpFs->ExtraTableSize = extratablesize;
	SpFs->Table = table;
	SpFs->TableStr = tablestr;
	SpFs->FilenameCount = filenamecount;
	SpFs->Filenames = filenames;
	SpFs->FileInfo = fileinfo;
	SpFs->UsedBlocks = usedblocks;

	// Need to save SpaceFS to SpFs ^

	if (NT_SUCCESS(FindDuplicate(SpFs, PWSTR(L""))))
	{
		createfile(PWSTR(L""), 545, 545, 448, 0, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);
	}

	if (NT_SUCCESS(FindDuplicate(SpFs, PWSTR(L"/"))))
	{
		winattrs = FILE_ATTRIBUTE_DIRECTORY;
		attrtoATTR(winattrs);
		createfile(PWSTR(L"/"), 545, 545, 16877, winattrs, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);
	}

	if (NT_SUCCESS(FindDuplicate(SpFs, PWSTR(L"?"))))
	{
		createfile(PWSTR(L"?"), 545, 545, 16877, 0, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);
	}
	else
	{
		std::cout << "Careful the disk was unmounted improperly." << std::endl;
	}

	if (NT_SUCCESS(FindDuplicate(SpFs, PWSTR(L":"))))
	{
		createfile(PWSTR(L":"), 545, 545, 448, 0, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);
	}

	// Create the root directory if needed ^

	getfilenameindex(PWSTR(L""), SpFs->Filenames, SpFs->FilenameCount, filenameindex, filenamestrindex);
	index = gettablestrindex(PWSTR(L""), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilesize(SpFs->SectorSize, index, SpFs->TableStr, filesize);
	if (!filesize)
	{
		char* buf = (char*)calloc(23, 1);
		if (!buf)
		{
			Result = STATUS_INSUFFICIENT_RESOURCES;
			goto exit;
		}
		memcpy(buf, "O:WDG:WDD:P(A;;FA;;;WD)", 23);
		trunfile(SpFs->hDisk, SpFs->SectorSize, index, SpFs->TableSize, SpFs->DiskSize, 0, 23, filenameindex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L""), SpFs->Filenames, SpFs->FilenameCount);
		readwritefile(SpFs->hDisk, SpFs->SectorSize, index, 0, 23, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, filenameindex, 1);
	}

	filenameindex = 0;
	filenamestrindex = 0;
	getfilenameindex(PWSTR(L"/"), SpFs->Filenames, SpFs->FilenameCount, filenameindex, filenamestrindex);
	index = gettablestrindex(PWSTR(L"/"), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilesize(SpFs->SectorSize, index, SpFs->TableStr, filesize);
	if (!trunfile(SpFs->hDisk, SpFs->SectorSize, index, SpFs->TableSize, SpFs->DiskSize, filesize, filesize + 1, filenameindex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L"/"), SpFs->Filenames, SpFs->FilenameCount))
	{
		trunfile(SpFs->hDisk, SpFs->SectorSize, index, SpFs->TableSize, SpFs->DiskSize, filesize + 1, filesize, filenameindex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L"/"), SpFs->Filenames, SpFs->FilenameCount);
	}

	filenameindex = 0;
	filenamestrindex = 0;
	getfilenameindex(PWSTR(L":"), SpFs->Filenames, SpFs->FilenameCount, filenameindex, filenamestrindex);
	index = gettablestrindex(PWSTR(L":"), SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilesize(SpFs->SectorSize, index, SpFs->TableStr, filesize);
	if (!filesize)
	{
		char* buf = (char*)calloc(8, 1);
		if (!buf)
		{
			Result = STATUS_INSUFFICIENT_RESOURCES;
			goto exit;
		}
		memcpy(buf, "CSpaceFS", 8);
		trunfile(SpFs->hDisk, SpFs->SectorSize, index, SpFs->TableSize, SpFs->DiskSize, 0, 8, filenameindex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks, PWSTR(L":"), SpFs->Filenames, SpFs->FilenameCount);
		readwritefile(SpFs->hDisk, SpFs->SectorSize, index, 0, 8, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, filenameindex, 1);
	}

	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table);

	// Init the root directory ^

	if (sectorsize / 512 > 32768)
	{
		unsigned long s = sectorsize;
		o = 0;
		while (s > 32768)
		{
			s >>= 1;
			o++;
		}
	}

	// Calculate the sector size larger than UINT16 ^

	FILETIME ltime;
	GetSystemTimeAsFileTime(&ltime);

	memset(&VolumeParams, 0, sizeof(VolumeParams));
	VolumeParams.SectorSize = min(1 << o, 32768);
	VolumeParams.SectorsPerAllocationUnit = min(sectorsize / 512, 32768);
	VolumeParams.MaxComponentLength = 0;
	VolumeParams.VolumeCreationTime = ((PLARGE_INTEGER)&ltime)->QuadPart;
	VolumeParams.VolumeSerialNumber = 0;
	VolumeParams.FileInfoTimeout = 1000;
	VolumeParams.CaseSensitiveSearch = 1;
	VolumeParams.CasePreservedNames = 1;
	VolumeParams.UnicodeOnDisk = 1;
	VolumeParams.PersistentAcls = 1;
	VolumeParams.ReparsePoints = 1;
	VolumeParams.NamedStreams = 1;
	VolumeParams.ExtendedAttributes = 1;
	VolumeParams.PostCleanupWhenModifiedOnly = 1;
	VolumeParams.PostDispositionWhenNecessaryOnly = 1;
	VolumeParams.PassQueryDirectoryPattern = 1;
	VolumeParams.FlushAndPurgeOnCleanup = 1;
	VolumeParams.DeviceControl = 1;
	VolumeParams.WslFeatures = 1;
	VolumeParams.AllowOpenInKernelMode = 1;
	VolumeParams.RejectIrpPriorToTransact0 = 1;
	VolumeParams.UmFileContextIsUserContext2 = 1;
	wcscpy_s(VolumeParams.FileSystemName, sizeof(VolumeParams.FileSystemName) / sizeof(WCHAR), PROGNAME);

	Result = FspFileSystemCreate(PWSTR(L"" FSP_FSCTL_DISK_DEVICE_NAME), &VolumeParams, &SpFsInterface, &SpFs->FileSystem);
	if (!NT_SUCCESS(Result))
	{
		goto exit;
	}
	SpFs->FileSystem->UserContext = SpFs;

	Result = FspFileSystemSetMountPoint(SpFs->FileSystem, MountPoint);
	if (!NT_SUCCESS(Result))
	{
		goto exit;
	}

	FspFileSystemSetDebugLog(SpFs->FileSystem, DebugFlags);

	Result = STATUS_SUCCESS;

exit:
	if (NT_SUCCESS(Result))
	{
		*PSpFs = SpFs;
	}
	else if (SpFs)
	{
		SpFsDelete(SpFs);
	}
	return Result;
}

static NTSTATUS EnableBackupRestorePrivileges(VOID)
{
	union
	{
		TOKEN_PRIVILEGES P;
		UINT8 B[sizeof(TOKEN_PRIVILEGES) + sizeof(LUID_AND_ATTRIBUTES)];
	} Privileges{};
	HANDLE Token;

	Privileges.P.PrivilegeCount = 2;
	Privileges.P.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	Privileges.P.Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;

	if (!LookupPrivilegeValueW(0, SE_BACKUP_NAME, &Privileges.P.Privileges[0].Luid) || !LookupPrivilegeValueW(0, SE_RESTORE_NAME, &Privileges.P.Privileges[1].Luid))
	{
		return FspNtStatusFromWin32(GetLastError());
	}

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
	{
		return FspNtStatusFromWin32(GetLastError());
	}

	if (!AdjustTokenPrivileges(Token, FALSE, &Privileges.P, 0, 0, 0))
	{
		CloseHandle(Token);

		return FspNtStatusFromWin32(GetLastError());
	}

	CloseHandle(Token);

	return STATUS_SUCCESS;
}

static ULONG wcstol_deflt(wchar_t* w, ULONG deflt)
{
	wchar_t* endp;
	ULONG ul = wcstol(w, &endp, 0);
	return L'\0' != w[0] && L'\0' == *endp ? ul : deflt;
}

static
NTSTATUS SvcStart(FSP_SERVICE* Service, ULONG argc, PWSTR* argv)
{
#define argtos(v) if (arge > ++argp) v = *argp; else goto usage
#define argtol(v) if (arge > ++argp) v = wcstol_deflt(*argp, v); else goto usage

	wchar_t** argp, ** arge;
	PWSTR Path = 0;
	PWSTR MountPoint = 0;
	ULONG SectorSize = 0;
	ULONG DebugFlags = 0;
	PWSTR DebugLogFile = 0;
	HANDLE DebugLogHandle = INVALID_HANDLE_VALUE;
	NTSTATUS Result = 0;
	SPFS* SpFs = 0;

	for (argp = argv + 1, arge = argv + argc; arge > argp; argp++)
	{
		if (L'-' != argp[0][0])
		{
			break;
		}
		switch (argp[0][1])
		{
		case L'?':
			goto usage;
		case L'd':
			argtol(DebugFlags);
			break;
		case L'D':
			argtos(DebugLogFile);
			break;
		case L'p':
			argtos(Path);
			break;
		case L'm':
			argtos(MountPoint);
			break;
		case L's':
			argtol(SectorSize);
			break;
		default:
			goto usage;
		}
	}

	if (arge > argp)
	{
		goto usage;
	}

	if (!Path || !MountPoint)
	{
		goto usage;
	}

	if (DebugLogFile)
	{
		if (!wcscmp(L"-", DebugLogFile))
		{
			DebugLogHandle = GetStdHandle(STD_ERROR_HANDLE);
		}
		else
		{
			DebugLogHandle = CreateFileW(DebugLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
		}
		if (INVALID_HANDLE_VALUE == DebugLogHandle)
		{
			fail((PWSTR)L"Was unable to open debug log file.");
			goto usage;
		}

		FspDebugLogSetHandle(DebugLogHandle);
	}

	EnableBackupRestorePrivileges();

	Result = SpFsCreate(Path, MountPoint, SectorSize, DebugFlags, &SpFs);
	if (!NT_SUCCESS(Result))
	{
		fail((PWSTR)L"Was unable to read/write file or drive.");
		goto exit;
	}

	FspFileSystemSetOperationGuardStrategy(SpFs->FileSystem, FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_COARSE);

	Result = FspFileSystemStartDispatcher(SpFs->FileSystem, 0);
	if (!NT_SUCCESS(Result))
	{
		fail((PWSTR)L"Was unable to start dispatcher.");
		goto exit;
	}

	MountPoint = FspFileSystemMountPoint(SpFs->FileSystem);

	Service->UserContext = SpFs;
	Result = STATUS_SUCCESS;

exit:
	if (!NT_SUCCESS(Result) && SpFs)
	{
		SpFsDelete(SpFs);
	}
	return Result;

usage:
	static wchar_t usage[] = L""
		"usage: %s OPTIONS\n"
		"\n"
		"options:\n"
		"    -d DebugFlags   [-1: enable all debug logs]\n"
		"    -D DebugLogFile [file path; use - for stderr]\n"
		"    -p Path         [file or drive to use as file system]\n"
		"    -m MountPoint   [X:|*|directory]\n"
		"    -s SectorSize   [used to specify to format and new sectorsize]\n";

	fail(usage, PROGNAME);
	return STATUS_UNSUCCESSFUL;

#undef argtos
#undef argtol
}

static
NTSTATUS SvcStop(FSP_SERVICE* Service)
{
	SPFS* SpFs = (SPFS*)Service->UserContext;
	FspFileSystemStopDispatcher(SpFs->FileSystem);
	SpFsDelete(SpFs);
	return STATUS_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
	if (!NT_SUCCESS(FspLoad(0)))
	{
		return ERROR_DELAY_LOAD_FAILED;
	}
	return FspServiceRun(PROGNAME, SvcStart, SvcStop, 0);
}