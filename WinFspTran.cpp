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

static void attrtoATTR(unsigned long& attr)
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

static void ATTRtoattr(unsigned long& ATTR)
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

static void ReplaceBSWFS(PWSTR& FileName)
{
	wchar_t* NewFileName = (wchar_t*)calloc(wcslen(FileName), sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	for (int i = 0; i < 256; i++)
	{
		if (FileName[i] == '\\')
			NewFileName[i] = '/';
		else
			NewFileName[i] = FileName[i];
		if (FileName[i] == '\0')
			break;
	}

	FileName = NewFileName;
}

static void RemoveFirst(PWSTR& FileName)
{
	wchar_t* NewFileName = (wchar_t*)calloc(wcslen(FileName), sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	int i = 0;
	for (; i < 256; i++)
	{
		NewFileName[i] = FileName[i + 1];
		if (FileName[i + 1] == '\0')
		{
			break;
		}
	}

	memcpy(FileName, NewFileName, (static_cast<unsigned long long>(i) + 1) * sizeof(wchar_t));
	free(NewFileName);
}

static void GetParentName(PWSTR& FileName, PWSTR& Suffix)
{
	unsigned Loc = 0;
	wchar_t* NewFileName = (wchar_t*)calloc(wcslen(FileName) + 2, sizeof(wchar_t));
	if (!NewFileName)
	{
		return;
	}

	for (unsigned i = 0; i < 256; i++)
	{
		if (FileName[i] == '\0')
			break;
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
		wchar_t* NewSuffix = (wchar_t*)calloc(wcslen(FileName), sizeof(wchar_t));
		if (!NewSuffix)
			return;

		unsigned i = 0;
		for (; i < 256; i++)
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

	ReplaceBSWFS(FileName);

	if (PFileAttributes)
	{
		getfilenameindex(FileName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
		chwinattrs(SpFs->FileInfo, SpFs->FilenameCount, FilenameIndex, winattrs, 0);
		ATTRtoattr(winattrs);
		*PFileAttributes = winattrs;
	}

	if (PSecurityDescriptorSize)
	{
		PWSTR SecurityName = (PWSTR)calloc(256, sizeof(wchar_t));
		if (!SecurityName)
		{
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memcpy(SecurityName, FileName, wcslen(FileName) * sizeof(wchar_t));
		RemoveFirst(SecurityName);
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

	return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	ReplaceBSWFS(FileName);
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS Open(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	ULONG CreateFlags;
	SPFS_FILE_CONTEXT* FileContext;

	ReplaceBSWFS(FileName);
	FileContext = (SPFS_FILE_CONTEXT*)calloc(sizeof * FileContext, 1);
	if (!FileContext)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memset(FileContext, 0, sizeof * FileContext);

	CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
	if (CreateOptions & FILE_DELETE_ON_CLOSE)
	{
		CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;
	}

	FileContext->Path = FileName;
	*PFileContext = FileContext;

	return GetFileInfoInternal(SpFs, FileInfo, FileName);
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static VOID Cleanup(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags)
{
	return;
}

static VOID Close(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext)
{
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
	GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
	free(Buf);

	return STATUS_SUCCESS;
}

static NTSTATUS Flush(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);

	return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;

	return GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT32 FileAttributes, UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime, FSP_FSCTL_FILE_INFO* FileInfo)
{
	return STATUS_INVALID_DEVICE_REQUEST;
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

	GetFileInfoInternal(SpFs, FileInfo, FileCtx->Path);
	return STATUS_SUCCESS;
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName)
{
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS Rename(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;

	ReplaceBSWFS(FileName);
	ReplaceBSWFS(NewFileName);
	getfilenameindex(FileName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);
	renamefile(FileName, NewFileName, FilenameSTRIndex, SpFs->Filenames);

	PWSTR SecurityName = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!SecurityName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileName, wcslen(FileName) * sizeof(wchar_t));
	RemoveFirst(SecurityName);
	getfilenameindex(SecurityName, SpFs->Filenames, SpFs->FilenameCount, FilenameIndex, FilenameSTRIndex);

	PWSTR NewSecurityName = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!NewSecurityName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(NewSecurityName, NewFileName, wcslen(NewFileName) * sizeof(wchar_t));
	RemoveFirst(NewSecurityName);
	renamefile(SecurityName, NewSecurityName, FilenameSTRIndex, SpFs->Filenames);

	free(SecurityName);
	free(NewSecurityName);
	simptable(SpFs->hDisk, SpFs->SectorSize, charmap, SpFs->TableSize, SpFs->ExtraTableSize, SpFs->FilenameCount, SpFs->FileInfo, SpFs->Filenames, SpFs->TableStr, SpFs->Table, emap, dmap);

	return STATUS_SUCCESS;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
	SPFS* SpFs = (SPFS*)FileSystem->UserContext;
	SPFS_FILE_CONTEXT* FileCtx = (SPFS_FILE_CONTEXT*)FileContext;
	PWSTR SecurityName = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!SecurityName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memcpy(SecurityName, FileCtx->Path, wcslen(FileCtx->Path) * sizeof(wchar_t));

	unsigned long long FilenameIndex = 0;
	unsigned long long FilenameSTRIndex = 0;

	RemoveFirst(SecurityName);
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
	PWSTR DirectoryName = FileCtx->Path;
	PWSTR ParentDirectoryName = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!ParentDirectoryName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	memcpy(ParentDirectoryName, DirectoryName, wcslen(DirectoryName) * sizeof(wchar_t));
	GetParentName(ParentDirectoryName, Suffix);

	if (DirectoryName[1] != L'\0')
	{
		if (!AddDirInfo(SpFs, DirectoryName, (PWSTR)L".", Buffer, BufferLength, PBytesTransferred))
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
	PWSTR FileName = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!FileName)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameParent = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!FileNameParent)
	{
		free(FileName);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	PWSTR FileNameSuffix = (PWSTR)calloc(256, sizeof(wchar_t));
	if (!FileNameSuffix)
	{
		free(FileName);
		free(FileNameParent);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	for (unsigned long long i = 0; i < SpFs->FilenameCount; i++)
	{
		unsigned long long j = 0;
		for (; j < 256; j++)
		{
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
		if (!wcscmp(FileNameParent, DirectoryName) && wcslen(FileNameParent) == wcslen(DirectoryName))
		{
			if (wcslen(FileNameSuffix) > 0)
			{
				if (!AddDirInfo(SpFs, FileName, FileNameSuffix, Buffer, BufferLength, PBytesTransferred))
				{
					return STATUS_SUCCESS;
				}
			}
		}
	}

	FspFileSystemAddDirInfo(0, Buffer, BufferLength, PBytesTransferred);

	free(FileNameParent);
	free(FileNameSuffix);
	free(FileName);

	return STATUS_SUCCESS;
}

static NTSTATUS ResolveReparsePoints(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 ReparsePointIndex, BOOLEAN ResolveLastPathComponent, PIO_STATUS_BLOCK PIoStatus, PVOID Buffer, PSIZE_T PSize)
{
	ReplaceBSWFS(FileName);
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
	NTSTATUS Result;

	*PSpFs = 0;

	Handle = CreateFileW(
		Path, FILE_READ_ATTRIBUTES, 0, 0,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
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
	SpFs = (SPFS*)malloc(sizeof * SpFs);
	if (!SpFs)
	{
		Result = STATUS_INSUFFICIENT_RESOURCES;
		goto exit;
	}
	memset(SpFs, 0, sizeof * SpFs);

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
	NTSTATUS Result;
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