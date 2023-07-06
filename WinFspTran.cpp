#include <winfsp/winfsp.h>
#include <sddl.h>
#include "SpaceFS.h"

#define PROGNAME PWSTR(L"CSpaceFS")

#define FULLPATH_SIZE (MAX_PATH + FSP_FSCTL_TRANSACT_PATH_SIZEMAX / sizeof(WCHAR))

#define info(format, ...) FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...) FspServiceLog(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...) FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

char* charmap = (char*)"0123456789-,.; ";
std::map<unsigned, unsigned> emap = {};
std::map<unsigned, unsigned> dmap = {};

typedef struct
{
	FSP_FILE_SYSTEM* FileSystem;
	PWSTR Path;
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
	wchar_t* NewFileName = (wchar_t*)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	unsigned long long i = 0;
	for (; i < wcslen(FileName); i++)
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
	wchar_t* NewFileName = (wchar_t*)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	unsigned long long i = 0;
	for (; i < wcslen(FileName); i++)
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

static VOID RemoveStream(PWSTR& FileName)
{
	for (unsigned long long i = 0; i < wcslen(FileName); i++)
	{
		if (FileName[i] == '\0' || FileName[i] == ':')
		{
			FileName[i] = '\0';
			break;
		}
	}
}

static VOID GetParentName(PWSTR& FileName, PWSTR& Suffix)
{
	unsigned Loc = 0;
	wchar_t* NewFileName = (wchar_t*)calloc(wcslen(FileName) + 2, sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	for (unsigned i = 0; i < wcslen(FileName); i++)
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
		wchar_t* NewSuffix = (wchar_t*)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
		if (!NewSuffix)
		{
			return;
		}

		unsigned i = 0;
		for (; i < wcslen(FileName); i++)
		{
			if (FileName[Loc + i + 1] == '\0')
			{
				break;
			}
			NewSuffix[i] = FileName[Loc + i + 1];
		}

		memcpy(Suffix, NewSuffix, (static_cast<unsigned long long>(i) + 1) * sizeof(wchar_t));
		free(NewSuffix);
	}

	if (Loc)
	{
		NewFileName[Loc] = '\0';
	}
	NewFileName[Loc + 1] = '\0';
	memcpy(FileName, NewFileName, (static_cast<unsigned long long>(Loc) + 2) * sizeof(wchar_t));
	free(NewFileName);
}

static NTSTATUS FindDuplicate(SPFS* SpFs, PWSTR FileName)
{
	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileName), FileNameLenT = wcslen(FileName);
	PWSTR ALC = NULL;
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			Filename[j] = SpFs->Filenames[Offset + j] & 0xff;
			if (j > FileNameLen - 1)
			{
				FileNameLen += 0xff;
				ALC = (PWSTR)realloc(Filename, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				Filename = ALC;
				ALC = NULL;
			}
		}
		Filename[j] = 0;

		if (!wcsincmp(Filename, FileName, FileNameLenT) && wcslen(Filename) == FileNameLenT)
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

	getfilenameindex(FileName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	chtime(SpFs->FileInfo, FilenameIndex, LastAccessTime, 0);
	chtime(SpFs->FileInfo, FilenameIndex, LastWriteTime, 2);
	chtime(SpFs->FileInfo, FilenameIndex, CreationTime, 4);

	ATTRtoattr(winattrs);
	FileInfo->FileAttributes = winattrs;
	FileInfo->ReparseTag = 0;
	FileInfo->FileSize = FileSize;
	FileInfo->AllocationSize = (FileSize + SpFs->SectorSize - 1) / SpFs->SectorSize * SpFs->SectorSize;
	FileInfo->CreationTime = CreationTime * 10000000 + 116444736000000000;
	FileInfo->LastAccessTime = LastAccessTime * 10000000 + 116444736000000000;
	FileInfo->LastWriteTime = LastWriteTime * 10000000 + 116444736000000000;
	FileInfo->ChangeTime = FileInfo->LastWriteTime;
	FileInfo->IndexNumber = FilenameIndex;
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
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameSTRIndex, 0);

	VolumeInfo->TotalSize = SpFs->DiskSize - SpFs->ExtraTableSize;
	VolumeInfo->FreeSize = SpFs->DiskSize - SpFs->ExtraTableSize - SpFs->UsedBlocks * SpFs->SectorSize;
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

	getfilenameindex(PWSTR(L":"), SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, wcslen(Label), FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks);
	char* buf = (char*)calloc(32, 1);
	if (!buf)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	for (int i = 0; i < wcslen(Label); i++)
	{
		buf[i] = Label[i];
	}
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, wcslen(Label), SpFs->DiskSize, SpFs->TableStr, buf, SpFs->FileInfo, FilenameIndex, 1);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);

	VolumeInfo->TotalSize = SpFs->DiskSize - SpFs->ExtraTableSize;
	VolumeInfo->FreeSize = SpFs->DiskSize - SpFs->ExtraTableSize - SpFs->UsedBlocks * SpFs->SectorSize;
	memcpy(VolumeInfo->VolumeLabel, Label, wcslen(Label) * sizeof(wchar_t));
	VolumeInfo->VolumeLabelLength = wcslen(Label) * sizeof(wchar_t);
	free(buf);

	return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long winattrs = 0;
	PWSTR Filename = (PWSTR)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, wcslen(FileName) * sizeof(wchar_t));
	ReplaceBSWFS(Filename);

	if (PFileAttributes)
	{
		getfilenameindex(Filename, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
		ATTRtoattr(winattrs);
		*PFileAttributes = winattrs;
	}

	if (PSecurityDescriptorSize)
	{
		PWSTR SecurityName = (PWSTR)calloc(wcslen(Filename) + 1, sizeof(wchar_t));
		if (!SecurityName)
		{
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memcpy(SecurityName, Filename, wcslen(Filename) * sizeof(wchar_t));
		RemoveFirst(SecurityName);
		RemoveStream(SecurityName);
		PSECURITY_DESCRIPTOR S = calloc(*PSecurityDescriptorSize, 1);
		getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		unsigned long long Index = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		free(SecurityName);
		unsigned long long FileSize = 0;
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		char* buf = (char*)calloc(FileSize + 1, 1);
		readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, (char*&)buf, SpFs->FileInfo, FilenameIndex, 0);
		ConvertStringSecurityDescriptorToSecurityDescriptorA(buf, SDDL_REVISION_1, &S, (PULONG)PSecurityDescriptorSize);
		memcpy(SecurityDescriptor, S, *PSecurityDescriptorSize);
		free(buf);
	}

	free(Filename);

	return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	ULONG CreateFlags;
	SPFS_FILE_CONTEXT* FileContext;
	NTSTATUS Result = 0;
	PWSTR Filename = (PWSTR)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, wcslen(FileName) * sizeof(wchar_t));
	ReplaceBSWFS(Filename);

	Result = FindDuplicate(SpFs, Filename);
	if (Result != STATUS_SUCCESS)
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

	CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
	if (CreateOptions & FILE_DELETE_ON_CLOSE)
	{
		CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;
	}

	FileContext->Path = Filename;
	*PFileContext = FileContext;

	PWSTR ParentDirectory = (PWSTR)calloc(wcslen(Filename) + 1, sizeof(wchar_t));
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

	memcpy(ParentDirectory, Filename, wcslen(Filename) * sizeof(wchar_t));
	GetParentName(ParentDirectory, Suffix);
	unsigned long long ParentIndex = gettablestrindex(ParentDirectory, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	getfilenameindex(ParentDirectory, SpFs->Filenames, SpFs->FilenameCount, ParentDirectoryIndex, ParentDirectorySTRIndex);
	chgid(SpFs->FileInfo, SpFs->FilenameCount, ParentDirectoryIndex, gid, 0);
	chuid(SpFs->FileInfo, SpFs->FilenameCount, ParentDirectoryIndex, uid, 0);
	PWSTR SecurityParentName = (PWSTR)calloc(wcslen(ParentDirectory) + 1, sizeof(wchar_t));
	if (!SecurityParentName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityParentName, ParentDirectory, wcslen(ParentDirectory) * sizeof(wchar_t));
	RemoveFirst(SecurityParentName);
	free(ParentDirectory);

	createfile(Filename, gid, uid, 448 + (FileAttributes & FILE_ATTRIBUTE_DIRECTORY) * 16429, winattrs, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);

	if (std::wstring(Filename).find(L":") == std::string::npos)
	{
		PWSTR SecurityName = (PWSTR)calloc(wcslen(Filename) + 1, sizeof(wchar_t));
		if (!SecurityName)
		{
			free(SecurityParentName);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memcpy(SecurityName, Filename, wcslen(Filename) * sizeof(wchar_t));
		RemoveFirst(SecurityName);

		PULONG BufLen = (PULONG)calloc(sizeof(PULONG), 1);
		if (!BufLen)
		{
			free(SecurityParentName);
			free(SecurityName);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		*BufLen = 65536;
		LPSTR* Buf = (LPSTR*)calloc(static_cast<size_t>(*BufLen) + 1, 1);
		if (!Buf)
		{
			free(SecurityParentName);
			free(SecurityName);
			free(BufLen);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		unsigned long long SecurityIndex = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		unsigned long long SecurityFileIndex = 0;
		unsigned long long SecurityFileSTRIndex = 0;

		createfile(SecurityName, gid, uid, 448, 2048, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, charmap, SpFs->TableStr);
		getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, SecurityFileIndex, SecurityFileSTRIndex);
		ConvertSecurityDescriptorToStringSecurityDescriptorA(SecurityDescriptor, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, Buf, BufLen);
		if (*BufLen > 65536)
		{
			free(Buf);
			Buf = (LPSTR*)calloc(static_cast<size_t>(*BufLen) + 1, 1);
			if (!Buf)
			{
				free(SecurityParentName);
				free(SecurityName);
				free(BufLen);
				return STATUS_INSUFFICIENT_RESOURCES;
			}
			ConvertSecurityDescriptorToStringSecurityDescriptorA(SecurityDescriptor, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, Buf, BufLen);
		}
		if (std::string(*Buf).find("D:P") == std::string::npos)
		{
			unsigned long long FileSize = 0;
			unsigned long long SecurityParentIndex = gettablestrindex(SecurityParentName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
			unsigned long long SecurityParentDirectoryIndex = 0;
			unsigned long long SecurityParentDirectorySTRIndex = 0;
			getfilenameindex(SecurityParentName, SpFs->Filenames, SpFs->FilenameCount, SecurityParentDirectoryIndex, SecurityParentDirectorySTRIndex);
			getfilesize(SpFs->SectorSize, SecurityParentIndex, SpFs->TableStr, FileSize);
			*BufLen = FileSize;
			LPSTR* ALC = (LPSTR*)realloc(Buf, static_cast<size_t>(*BufLen) + 1);
			if (!ALC)
			{
				free(SecurityParentName);
				free(SecurityName);
				free(BufLen);
				free(Buf);
				return STATUS_INSUFFICIENT_RESOURCES;
			}
			Buf = ALC;
			ALC = NULL;
			readwritefile(SpFs->hDisk, SpFs->SectorSize, SecurityParentIndex, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, *Buf, SpFs->FileInfo, SecurityParentDirectoryIndex, 0);
		}
		trunfile(SpFs->hDisk, SpFs->SectorSize, SecurityIndex, SpFs->TableSize, SpFs->DiskSize, 0, *BufLen, SecurityFileIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks);
		readwritefile(SpFs->hDisk, SpFs->SectorSize, SecurityIndex, 0, *BufLen, SpFs->DiskSize, SpFs->TableStr, *Buf, SpFs->FileInfo, SecurityFileIndex, 1);
		free(SecurityName);
		free(BufLen);
		free(Buf);
	}

	free(SecurityParentName);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);

	return GetFileInfoInternal(SpFs, FileInfo, Filename);
}

static NTSTATUS Open(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	ULONG CreateFlags;
	SPFS_FILE_CONTEXT* FileContext;
	PWSTR Filename = (PWSTR)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, wcslen(FileName) * sizeof(wchar_t));
	ReplaceBSWFS(Filename);
	FileContext = (SPFS_FILE_CONTEXT*)calloc(sizeof(*FileContext), 1);
	if (!FileContext)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memset(FileContext, 0, sizeof(*FileContext));

	CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
	if (CreateOptions & FILE_DELETE_ON_CLOSE)
	{
		CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;
	}

	FileContext->Path = Filename;
	*PFileContext = FileContext;

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
	NTSTATUS Result = 0;

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);

	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = wcslen(FileCtx->Path);
	PWSTR ALC = NULL;
	PWSTR TempFilename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!TempFilename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	unsigned long long O = 0;
	PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileNameNoStream)
	{
		free(TempFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (unsigned long long i = 0; i < SpFs->FilenameCount + O; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			TempFilename[j] = SpFs->Filenames[Offset + j] & 0xff;
			if (j > FileNameLen - 1)
			{
				FileNameLen += 0xff;
				ALC = (PWSTR)realloc(TempFilename, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(TempFilename);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				TempFilename = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameNoStream, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(TempFilename);
					free(FileNameNoStream);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameNoStream = ALC;
				ALC = NULL;
			}
		}
		TempFilename[j] = 0;

		memcpy(FileNameNoStream, TempFilename, (j + 1) * sizeof(wchar_t));
		RemoveStream(FileNameNoStream);
		if (!wcsincmp(FileNameNoStream, FileCtx->Path, FileNameLenT) && wcslen(FileNameNoStream) == FileNameLenT)
		{
			if (std::wstring(TempFilename).find(L":") != std::string::npos)
			{
				getfilenameindex(TempFilename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
				TempIndex = gettablestrindex(TempFilename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
				deletefile(TempIndex, TempFilenameIndex, TempFilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
				Offset -= wcslen(TempFilename) + 1;
				O++;
			}
		}
	}

	free(TempFilename);
	free(FileNameNoStream);

	if (ReplaceFileAttributes)
	{
		unsigned long winattrs = FileAttributes | FILE_ATTRIBUTE_ARCHIVE;
		ATTRtoattr(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	}
	else
	{
		unsigned long winattrs = 0;
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
		attrtoATTR(winattrs);
		winattrs |= FileAttributes | FILE_ATTRIBUTE_ARCHIVE;
		ATTRtoattr(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	}

	time_t ltime;
	time(&ltime);
	double LTime = (double)ltime;
	chtime(SpFs->FileInfo, FilenameIndex, LTime, 1);
	chtime(SpFs->FileInfo, FilenameIndex, LTime, 3);
	chtime(SpFs->FileInfo, FilenameIndex, LTime, 5);

	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);
	Result = GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);

	return Result;
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

	if (Flags & FspCleanupSetArchiveBit)
	{
		if (!(winattrs & FILE_ATTRIBUTE_DIRECTORY))
		{
			winattrs |= FILE_ATTRIBUTE_ARCHIVE;
			attrtoATTR(winattrs);
			chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
		}
	}

	time_t ltime;
	time(&ltime);

	if (Flags & FspCleanupSetLastAccessTime)
	{
		double ATime = (double)ltime;
		chtime(SpFs->FileInfo, FilenameIndex, ATime, 1);
	}

	if (Flags & FspCleanupSetLastWriteTime || Flags & FspCleanupSetChangeTime)
	{
		double WTime = (double)ltime;
		chtime(SpFs->FileInfo, FilenameIndex, WTime, 3);
	}

	if (Flags & FspCleanupDelete)
	{
		deletefile(Index, FilenameIndex, FilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
		Index = gettablestrindex(FileCtx->Path + 1, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
		getfilenameindex(FileCtx->Path + 1, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		deletefile(Index, FilenameIndex, FilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);

		unsigned long long TempIndex = 0;
		unsigned long long TempFilenameIndex = 0;
		unsigned long long TempFilenameSTRIndex = 0;
		unsigned long long Offset = 0;
		unsigned long long O = 0;
		unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = wcslen(FileCtx->Path);
		PWSTR ALC = NULL;
		PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
		if (!Filename)
		{
			return;
		}
		PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
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
				if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
				{
					Offset += j + 1;
					break;
				}
				Filename[j] = SpFs->Filenames[Offset + j] & 0xff;
				if (j > FileNameLen - 1)
				{
					FileNameLen += 0xff;
					ALC = (PWSTR)realloc(Filename, (FileNameLen + 1) * sizeof(wchar_t));
					if (!ALC)
					{
						free(Filename);
						free(FileNameNoStream);
						return;
					}
					Filename = ALC;
					ALC = NULL;
					ALC = (PWSTR)realloc(FileNameNoStream, (FileNameLen + 1) * sizeof(wchar_t));
					if (!ALC)
					{
						free(Filename);
						free(FileNameNoStream);
						return;
					}
					FileNameNoStream = ALC;
					ALC = NULL;
				}
			}
			Filename[j] = 0;

			memcpy(FileNameNoStream, Filename, (j + 1) * sizeof(wchar_t));
			RemoveStream(FileNameNoStream);
			if (!wcsincmp(FileNameNoStream, FileCtx->Path, FileNameLenT) && wcslen(FileNameNoStream) == FileNameLenT)
			{
				if (std::wstring(Filename).find(L":") != std::string::npos)
				{
					getfilenameindex(Filename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
					TempIndex = gettablestrindex(Filename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
					deletefile(TempIndex, TempFilenameIndex, TempFilenameSTRIndex, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr);
					Offset -= wcslen(Filename) + 1;
					O++;
				}
			}
		}

		free(Filename);
		free(FileNameNoStream);

		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);
	}

	return;
}

static VOID Close(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext)
{
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	free(FileCtx->Path);
	free(FileCtx);
	return;
}

static NTSTATUS Read(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, PULONG PBytesTransffered)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long FileSize = 0;

	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	if (Offset >= FileSize)
	{
		return STATUS_END_OF_FILE;
	}
	Length = min(Length, FileSize - Offset);
	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	char* Buf = (char*)calloc(static_cast<size_t>(Length) + 1, 1);
	if (!Buf)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, Offset, Length, SpFs->DiskSize, SpFs->TableStr, Buf, SpFs->FileInfo, FilenameIndex, 0);
	memcpy(Buffer, Buf, Length);
	*PBytesTransffered = Length;
	free(Buf);

	return STATUS_SUCCESS;
}

static NTSTATUS Write(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	NTSTATUS Result = 0;

	unsigned long long Index = gettablestrindex(FileCtx->Path, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;
	unsigned long long FileSize = 0;

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);

	if (WriteToEndOfFile)
	{
		Offset = FileSize;
	}
	if (Offset + Length > FileSize)
	{
		trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, Offset + Length, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks);
		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);
	}

	char* Buf = (char*)calloc(static_cast<size_t>(Length) + 1, 1);
	if (!Buf)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(Buf, Buffer, Length);
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, Offset, Length, SpFs->DiskSize, SpFs->TableStr, Buf, SpFs->FileInfo, FilenameIndex, 1);
	*PBytesTransferred = Length;
	Result = GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
	free(Buf);

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

	if (winattrs != INVALID_FILE_ATTRIBUTES)
	{
		ATTRtoattr(winattrs);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 1);
	}

	if (LastAccessTime)
	{
		double ATime = (LastAccessTime - static_cast<double>(116444736000000000)) / 10000000;
		chtime(SpFs->FileInfo, FilenameIndex, ATime, 1);
	}

	if (LastWriteTime || ChangeTime)
	{
		LastWriteTime = max(LastWriteTime, ChangeTime);
		double WTime = (LastWriteTime - static_cast<double>(116444736000000000)) / 10000000;
		chtime(SpFs->FileInfo, FilenameIndex, WTime, 3);
	}

	if (CreationTime)
	{
		double CTime = (CreationTime - static_cast<double>(116444736000000000)) / 10000000;
		chtime(SpFs->FileInfo, FilenameIndex, CTime, 5);
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
		unsigned long long FilenameIndex = 0;
		unsigned long long FilenameSTRIndex = 0;
		unsigned long long FileSize = 0;

		getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
		trunfile(SpFs->hDisk, SpFs->SectorSize, Index, SpFs->TableSize, SpFs->DiskSize, FileSize, NewSize, FilenameIndex, charmap, SpFs->TableStr, SpFs->FileInfo, SpFs->UsedBlocks);
		simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);
	}

	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = wcslen(FileCtx->Path);
	PWSTR ALC = NULL;
	PWSTR Filename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(Filename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
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
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			Filename[j] = SpFs->Filenames[Offset + j] & 0xff;
			if (j > FileNameLen - 1)
			{
				FileNameLen += 0xff;
				ALC = (PWSTR)realloc(Filename, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				Filename = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameParent, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(Filename);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameParent = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameSuffix, (FileNameLen + 1) * sizeof(wchar_t));
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
		}
		Filename[j] = 0;

		memcpy(FileNameParent, Filename, (j + 1) * sizeof(wchar_t));
		GetParentName(FileNameParent, FileNameSuffix);
		if (!wcsincmp(FileNameParent, FileCtx->Path, FileNameLenT) && wcslen(FileNameParent) == FileNameLenT)
		{
			if (wcslen(FileNameSuffix) > 0)
			{
				free(Filename);
				free(FileNameParent);
				free(FileNameSuffix);
				return STATUS_DIRECTORY_NOT_EMPTY;
			}
		}
	}

	return STATUS_SUCCESS;
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
	NTSTATUS Result = 0;

	PWSTR NewFilename = (PWSTR)calloc(wcslen(NewFileName) + 1, sizeof(wchar_t));
	if (!NewFilename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(NewFilename, NewFileName, wcslen(NewFileName) * sizeof(wchar_t));
	ReplaceBSWFS(NewFilename);

	Result = FindDuplicate(SpFs, NewFilename);
	if (Result != STATUS_SUCCESS)
	{
		free(NewFilename);
		return Result;
	}

	getfilenameindex(FileCtx->Path, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	renamefile(FileCtx->Path, NewFilename, FilenameSTRIndex, SpFs->Filenames);

	PWSTR SecurityName = (PWSTR)calloc(wcslen(FileCtx->Path) + 1, sizeof(wchar_t));
	if (!SecurityName)
	{
		free(NewFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileCtx->Path, wcslen(FileCtx->Path) * sizeof(wchar_t));
	RemoveFirst(SecurityName);
	RemoveStream(SecurityName);
	getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	PWSTR NewSecurityName = (PWSTR)calloc(wcslen(NewFilename) + 1, sizeof(wchar_t));
	if (!NewSecurityName)
	{
		free(NewFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(NewSecurityName, NewFilename, wcslen(NewFilename) * sizeof(wchar_t));
	RemoveFirst(NewSecurityName);
	RemoveStream(NewSecurityName);
	renamefile(SecurityName, NewSecurityName, FilenameSTRIndex, SpFs->Filenames);

	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = wcslen(FileCtx->Path);
	PWSTR ALC = NULL;
	PWSTR TempFilename = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!TempFilename)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		free(TempFilename);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
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
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			TempFilename[j] = SpFs->Filenames[Offset + j] & 0xff;
			if (j > FileNameLen - 1)
			{
				FileNameLen += 0xff;
				ALC = (PWSTR)realloc(TempFilename, (FileNameLen + 1) * sizeof(wchar_t));
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
				ALC = (PWSTR)realloc(FileNameParent, (FileNameLen + 1) * sizeof(wchar_t));
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
				ALC = (PWSTR)realloc(FileNameSuffix, (FileNameLen + 1) * sizeof(wchar_t));
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
		}
		TempFilename[j] = 0;

		memcpy(FileNameParent, TempFilename, (j + 1) * sizeof(wchar_t));
		GetParentName(FileNameParent, FileNameSuffix);
		if (!wcsincmp(FileNameParent, FileCtx->Path, FileNameLenT) && wcslen(FileNameParent) == FileNameLenT)
		{
			if (wcslen(FileNameSuffix) > 0)
			{
				getfilenameindex(TempFilename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
				renamefile(TempFilename, (PWSTR)(std::wstring(NewFilename) + L"/" + FileNameSuffix).c_str(), TempFilenameSTRIndex, SpFs->Filenames);
				getfilenameindex(TempFilename + 1, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
				renamefile(TempFilename + 1, (PWSTR)(std::wstring(NewFilename) + L"/" + FileNameSuffix).c_str() + 1, TempFilenameSTRIndex, SpFs->Filenames);
			}
		}
	}

	Offset = 0;
	unsigned long long O = 0;
	PWSTR FileNameNoStream = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
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

	for (unsigned long long i = 0; i < SpFs->FilenameCount + O; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			TempFilename[j] = SpFs->Filenames[Offset + j] & 0xff;
			if (j > FileNameLen - 1)
			{
				FileNameLen += 0xff;
				ALC = (PWSTR)realloc(TempFilename, (FileNameLen + 1) * sizeof(wchar_t));
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
				ALC = (PWSTR)realloc(FileNameNoStream, (FileNameLen + 1) * sizeof(wchar_t));
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
		}
		TempFilename[j] = 0;

		memcpy(FileNameNoStream, TempFilename, (j + 1) * sizeof(wchar_t));
		RemoveStream(FileNameNoStream);
		if (!wcsincmp(FileNameNoStream, FileCtx->Path, FileNameLenT) && wcslen(FileNameNoStream) == FileNameLenT)
		{
			if (std::wstring(TempFilename).find(L":") != std::string::npos)
			{
				getfilenameindex(TempFilename, SpFs->Filenames, SpFs->FilenameCount, TempFilenameIndex, TempFilenameSTRIndex);
				TempIndex = gettablestrindex(TempFilename, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
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

	ALC = (PWSTR)realloc(FileCtx->Path, (wcslen(NewFilename) + 1) * sizeof(wchar_t));
	if (!ALC)
	{
		free(NewFilename);
		free(SecurityName);
		free(NewSecurityName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	FileCtx->Path = ALC;
	ALC = NULL;
	FileCtx->Path[wcslen(NewFilename)] = 0;
	memcpy(FileCtx->Path, NewFilename, wcslen(NewFilename) * sizeof(wchar_t));
	free(NewFilename);
	free(SecurityName);
	free(NewSecurityName);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);

	return STATUS_SUCCESS;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	PWSTR SecurityName = (PWSTR)calloc(wcslen(FileCtx->Path) + 1, sizeof(wchar_t));
	if (!SecurityName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileCtx->Path, wcslen(FileCtx->Path) * sizeof(wchar_t));

	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;

	RemoveFirst(SecurityName);
	RemoveStream(SecurityName);
	PSECURITY_DESCRIPTOR S = calloc(*PSecurityDescriptorSize, 1);
	getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	unsigned long long Index = gettablestrindex(SecurityName, SpFs->Filenames, SpFs->TableStr, SpFs->FilenameCount);
	free(SecurityName);
	unsigned long long FileSize = 0;

	getfilesize(SpFs->SectorSize, Index, SpFs->TableStr, FileSize);
	char* buf = (char*)calloc(FileSize + 1, 1);
	readwritefile(SpFs->hDisk, SpFs->SectorSize, Index, 0, FileSize, SpFs->DiskSize, SpFs->TableStr, (char*&)buf, SpFs->FileInfo, FilenameIndex, 0);
	ConvertStringSecurityDescriptorToSecurityDescriptorA(buf, SDDL_REVISION_1, &S, (PULONG)PSecurityDescriptorSize);
	memcpy(SecurityDescriptor, S, *PSecurityDescriptorSize);
	free(buf);

	return STATUS_SUCCESS;
}

static NTSTATUS SetSecurity(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static BOOLEAN AddDirInfo(SPFS* SpFs, PWSTR Name, PWSTR FileName, PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
	FSP_FSCTL_DIR_INFO* DirInfo = (FSP_FSCTL_DIR_INFO*)calloc(sizeof(FSP_FSCTL_DIR_INFO) + (wcslen(FileName) + 1) * sizeof(wchar_t), 1);
	if (!DirInfo)
	{
		return FALSE;
	}

	memset(DirInfo->Padding, 0, sizeof DirInfo->Padding);
	DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + (wcslen(FileName) + 1) * sizeof(wchar_t));
	GetFileInfoInternal(SpFs, &DirInfo->FileInfo, Name);
	memcpy(DirInfo->FileNameBuf, FileName, DirInfo->Size - sizeof(FSP_FSCTL_DIR_INFO));
	BOOLEAN Result = FspFileSystemAddDirInfo(DirInfo, Buffer, Length, PBytesTransferred);

	free(DirInfo);
	return Result;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR Patter, PWSTR Marker, PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	PWSTR Suffix = 0;
	PWSTR ParentDirectoryName = (PWSTR)calloc(wcslen(FileCtx->Path) + 1, sizeof(wchar_t));
	if (!ParentDirectoryName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(ParentDirectoryName, FileCtx->Path, wcslen(FileCtx->Path) * sizeof(wchar_t));
	GetParentName(ParentDirectoryName, Suffix);

	if (FileCtx->Path[1] != L'\0')
	{
		if (!AddDirInfo(SpFs, FileCtx->Path, (PWSTR)L".", Buffer, BufferLength, PBytesTransferred))
		{
			free(ParentDirectoryName);
			return STATUS_SUCCESS;
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

	unsigned long long Offset = 0;
	unsigned long long FileNameLen = wcslen(FileCtx->Path), FileNameLenT = wcslen(FileCtx->Path);
	PWSTR ALC = NULL;
	PWSTR FileName = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(FileName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(FileNameLen + 1, sizeof(wchar_t));
	if (!FileNameSuffix)
	{
		free(FileName);
		free(FileNameParent);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (;; j++)
		{
			if ((SpFs->Filenames[Offset + j] & 0xff) == 255 || (SpFs->Filenames[Offset + j] & 0xff) == 42)
			{
				Offset += j + 1;
				break;
			}
			FileName[j] = SpFs->Filenames[Offset + j] & 0xff;
			if (j > FileNameLen - 1)
			{
				FileNameLen += 0xff;
				ALC = (PWSTR)realloc(FileName, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(FileName);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileName = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameParent, (FileNameLen + 1) * sizeof(wchar_t));
				if (!ALC)
				{
					free(FileName);
					free(FileNameParent);
					free(FileNameSuffix);
					return STATUS_INSUFFICIENT_RESOURCES;
				}
				FileNameParent = ALC;
				ALC = NULL;
				ALC = (PWSTR)realloc(FileNameSuffix, (FileNameLen + 1) * sizeof(wchar_t));
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
		}
		FileName[j] = 0;

		memcpy(FileNameParent, FileName, (j + 1) * sizeof(wchar_t));
		GetParentName(FileNameParent, FileNameSuffix);
		if (!wcsincmp(FileNameParent, FileCtx->Path, FileNameLenT) && wcslen(FileNameParent) == FileNameLenT)
		{
			if (wcslen(FileNameSuffix) > 0)
			{
				if (std::wstring(FileNameSuffix).find(L":") == std::string::npos)
				{
					if (!AddDirInfo(SpFs, FileName, FileNameSuffix, Buffer, BufferLength, PBytesTransferred))
					{
						return STATUS_SUCCESS;
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
	PWSTR Filename = (PWSTR)calloc(wcslen(FileName) + 1, sizeof(wchar_t));
	if (!Filename)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(Filename, FileName, wcslen(FileName) * sizeof(wchar_t));
	ReplaceBSWFS(Filename);
	free(Filename);
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS GetReparsePoint(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PVOID Buffer, PSIZE_T PSize)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS SetReparsePoint(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PVOID Buffer, SIZE_T Size)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS DeleteReparsePoint(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PVOID Buffer, SIZE_T Size)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS GetStreamInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
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
	0,
	0,
	0,
	0,
	0,
	GetEa,
	SetEa,
	0,
	DispatcherStopped,
};

static
VOID SpFsDelete(SPFS* SpFs)
{
	if (SpFs->FileSystem)
	{
		FspFileSystemDelete(SpFs->FileSystem);
	}

	if (SpFs->Path)
	{
		free(SpFs->Path);
	}

	if (SpFs->MountPoint)
	{
		free(SpFs->MountPoint);
	}

	free(SpFs);
}

static
NTSTATUS SpFsCreate(PWSTR Path, PWSTR MountPoint, UINT32 SectorSize, UINT32 DebugFlags, SPFS** PSpFs)
{
	WCHAR FullPath[MAX_PATH];
	ULONG Length;
	HANDLE Handle;
	FILETIME CreationTime;
	DWORD LastError;
	FSP_FSCTL_VOLUME_PARAMS VolumeParams;
	SPFS* SpFs = 0;
	NTSTATUS Result = 0;

	*PSpFs = 0;

	Handle = CreateFileW(Path, FILE_READ_ATTRIBUTES, 0, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return FspNtStatusFromWin32(GetLastError());
	}

	Length = GetFinalPathNameByHandleW(Handle, FullPath, MAX_PATH, 0);
	if (0 == Length)
	{
		LastError = GetLastError();
		CloseHandle(Handle);
		return FspNtStatusFromWin32(LastError);
	}
	if (L'\\' == FullPath[Length - 1])
	{
		FullPath[--Length] = L'\0';
	}

	if (!GetFileTime(Handle, &CreationTime, 0, 0))
	{
		LastError = GetLastError();
		CloseHandle(Handle);
		return FspNtStatusFromWin32(LastError);
	}

	CloseHandle(Handle);

	// From now on we must goto exit on failure.

	unsigned long sectorsize = 512;

	//std::cout << "Opening disk: " << argv[1] << std::endl;
	HANDLE hDisk = CreateFile(Path,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (hDisk == INVALID_HANDLE_VALUE)
	{
		std::cout << "Opening Error: " << GetLastError() << std::endl;
		return 1;
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
	unsigned long long index = 0;
	unsigned long long filenameindex = 0;
	unsigned long long filenamestrindex = 0;
	unsigned long long filesize = 0;
	getfilenameindex(PWSTR(L"/"), filenames, filenamecount, filenameindex, filenamestrindex);
	index = gettablestrindex(PWSTR(L"/"), filenames, tablestr, filenamecount);
	getfilesize(sectorsize, index, tablestr, filesize);
	trunfile(hDisk, sectorsize, index, tablesize, disksize.QuadPart, filesize, filesize + 1, filenameindex, charmap, tablestr, fileinfo, usedblocks);
	trunfile(hDisk, sectorsize, index, tablesize, disksize.QuadPart, filesize + 1, filesize, filenameindex, charmap, tablestr, fileinfo, usedblocks);

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

	Length = (static_cast<unsigned long long>(Length) + 1) * sizeof(WCHAR);
	SpFs->Path = (PWSTR)malloc(Length);
	if (!SpFs->Path)
	{
		Result = STATUS_INSUFFICIENT_RESOURCES;
		goto exit;
	}
	memcpy(SpFs->Path, FullPath, Length);

	memset(&VolumeParams, 0, sizeof VolumeParams);
	VolumeParams.SectorSize = min(1 << o, 32768);
	VolumeParams.SectorsPerAllocationUnit = min(sectorsize / 512, 32768);
	VolumeParams.MaxComponentLength = 255;
	VolumeParams.VolumeCreationTime = ((PLARGE_INTEGER)&CreationTime)->QuadPart;
	VolumeParams.VolumeSerialNumber = 0;
	VolumeParams.FileInfoTimeout = 1000;
	VolumeParams.CaseSensitiveSearch = 0;
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
	VolumeParams.WslFeatures = 1;
	VolumeParams.AllowOpenInKernelMode = 1;
	VolumeParams.RejectIrpPriorToTransact0 = 1;
	VolumeParams.UmFileContextIsUserContext2 = 1;
	wcscpy_s(VolumeParams.FileSystemName, sizeof VolumeParams.FileSystemName / sizeof(WCHAR), PROGNAME);

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
	NTSTATUS Result = 0;
	SPFS* SpFs = 0;

	for (argp = argv + 1, arge = argv + argc; arge > argp; argp++)
	{
		if (L'-' != argp[0][0])
			break;
		switch (argp[0][1])
		{
		case L'?':
			goto usage;
		case L'd':
			argtol(DebugFlags);
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

	EnableBackupRestorePrivileges();

	Result = SpFsCreate(Path, MountPoint, SectorSize, DebugFlags, &SpFs);
	if (!NT_SUCCESS(Result))
	{
		fail((PWSTR)L"Was unable to read/write file or drive.");
		goto exit;
	}

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
		"    -d DebugFlags [-1: enable all debug logs]\n"
		"    -p Path       [file or drive to use as file system]\n"
		"    -m MountPoint [X:|*|directory]\n"
		"    -s SectorSize [used to specify to format and new sectorsize]\n";

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