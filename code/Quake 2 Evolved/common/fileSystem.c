/*
 ------------------------------------------------------------------------------
 Copyright (C) 1997-2001 Id Software.

 This file is part of the Quake 2 source code.

 The Quake 2 source code is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or (at your
 option) any later version.

 The Quake 2 source code is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.

 You should have received a copy of the GNU General Public License along with
 the Quake 2 source code; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 ------------------------------------------------------------------------------
*/


//
// fileSystem.c - Handle based file system
//

// TODO:
// - fileInPath->name is wrong when loading from pk3 files (see FS_ListFiles)
// - FS_AddGameDirectory
// - directory crap?
// - console commands
// - Com_SafeMode in FS_Init


#include "common.h"
#include "../include/ZLib/unzip.h"


#define	BASE_DIRECTORY				"baseq2"

#define FILES_HASH_SIZE				1024

#define MAX_FILE_HANDLES			64

#define MAX_LIST_FILES				65536
#define MAX_LIST_MODS				64

#define MAX_PACK_FILES				1024

typedef struct {
	char				name[MAX_QPATH];
	fsMode_t			mode;

	FILE *				realFile;			// Only one of realFile or
	unzFile				zipFile;			// zipFile will be used
} file_t;

typedef struct fileInPack_s {
	char				name[MAX_QPATH];
	bool				isDirectory;

	int					size;
	int					offset;				// This is ignored in PK3 files

	struct fileInPack_s *nextHash;
} fileInPack_t;

typedef struct {
	char				name[MAX_OSPATH];
	
	FILE *				pak;				// Only one of pak or
	unzFile				pk3;				// pk3 will be used
	
	int					numFiles;
	fileInPack_t *		files;
	fileInPack_t *		filesHashTable[FILES_HASH_SIZE];
} pack_t;

typedef struct fsSearchPath_s {
	char				path[MAX_OSPATH];	// Only one of path or
	pack_t *			pack;				// pack will be used

	struct fsSearchPath_s *next;
} searchPath_t;

static file_t			fs_fileHandles[MAX_FILE_HANDLES];

static searchPath_t	*	fs_searchPaths;

static int				fs_readCount;
static int				fs_writeCount;

static char				fs_gameDirectory[MAX_OSPATH];
static char				fs_curGame[MAX_QPATH];

static cvar_t *			fs_debug;
static cvar_t *			fs_savePath;
cvar_t *				fs_game;


cvar_t *				fs_homePath;
static cvar_t *			fs_basePath;
static cvar_t *			fs_baseGame;


/*
 ==============================================================================

 INDEX FILE HANDLE

 ==============================================================================
*/


/*
 ==================
 FS_HandleForFile

 Finds a free fileHandle_t
 ==================
*/
static file_t *FS_HandleForFile (fileHandle_t *f){

	file_t	*file;
	int		i;

	for (i = 0, file = fs_fileHandles; i < MAX_FILE_HANDLES; i++, file++){
		if (!file->realFile && !file->zipFile)
			break;
	}

	if (i == MAX_FILE_HANDLES)
		Com_Error(ERR_FATAL, "FS_HandleForFile: none free");

	*f = i + 1;

	return file;
}

/*
 ==================
 FS_GetFileByHandle

 Returns a file_t for the given fileHandle_t
 ==================
*/
static file_t *FS_GetFileByHandle (fileHandle_t f){

	file_t	*file;

	if (f <= 0 || f > MAX_FILE_HANDLES)
		Com_Error(ERR_FATAL, "FS_GetFileByHandle: handle out of range");

	file = &fs_fileHandles[f - 1];

	if (!file->realFile && !file->zipFile)
		Com_Error(ERR_FATAL, "FS_GetFileByHandle: invalid handle");

	return file;
}


/*
 ==============================================================================

 MISCELLANEOUS FUNCTIONS

 ==============================================================================
*/


/*
 ==================
 FS_FileLength

 Returns the length of the given FILE
 ==================
*/
static int FS_FileLength (FILE *f){

	int		cur, end;

	cur = ftell(f);
	fseek(f, 0, SEEK_END);
	end = ftell(f);
	fseek(f, cur, SEEK_SET);

	return end;
}

/*
 ==================
 FS_BuildOSPath

 Builds a full OS path from the given components
 ==================
*/
static void FS_BuildOSPath (const char *base, const char *game, const char *name, char osPath[MAX_PATH_LENGTH]){

	Str_SPrintf(osPath, MAX_PATH_LENGTH, "%s/%s/%s", base, game, name);

	// Fix up path separators
	while (*osPath){
		if (*osPath == '/' || *osPath == '\\')
			*osPath = PATH_SEPARATOR_CHAR;

		osPath++;
	}
}

/*
 ==================
 FS_CreateOSPath

 Creates the given OS path if it doesn't exist
 ==================
*/
static void FS_CreateOSPath (char *osPath){

	char	*p;

	p = osPath + 1;

	while (*p){
		if (*p == PATH_SEPARATOR_CHAR){
			*p = 0;

			Sys_CreateDirectory(osPath);

			*p = PATH_SEPARATOR_CHAR;
		}

		p++;
	}
}

/*
 ==================
 FS_FileNameCompare

 Ignores case and separator char distinctions
 ==================
*/
static int FS_FileNameCompare (const char *name1, const char *name2){

	int		c1, c2;

	do {
		c1 = *name1++;
		c2 = *name2++;

		if (c1 != c2){
			if (c1 >= 'a' && c1 <= 'z')
				c1 -= ('a' - 'A');
			if (c2 >= 'a' && c2 <= 'z')
				c2 -= ('a' - 'A');

			if (c1 == ':' || c1 == '\\')
				c1 = '/';
			if (c2 == ':' || c2 == '\\')
				c2 = '/';

			if (c1 != c2){
				// Names not equal
				if (c1 < c2)
					return -1;
				else
					return 1;
			}
		}
	} while (c1);

	return 0;	// Names are equal
}


/*
 ==============================================================================

 FILE I/O FUNCTIONS

 ==============================================================================
*/


/*
 ==================
 FS_Read

 Properly handles partial reads
 ==================
*/
int FS_Read (fileHandle_t f, void *buffer, int size){

	file_t	*file;
	byte	*ptr;
	int		remaining, count;
	bool	tried = false;

	if (!buffer)
		Com_Error(ERR_FATAL, "FS_Read: NULL buffer");
	if (size < 0)
		Com_Error(ERR_FATAL, "FS_Read: size < 0");

	file = FS_GetFileByHandle(f);

	if (file->mode != FS_READ)
		Com_Error(ERR_FATAL, "FS_Read: cannot read from '%s'", file->name);

	ptr = (byte *)buffer;
	remaining = size;

	while (remaining){
		if (file->realFile)
			count = fread(ptr, 1, remaining, file->realFile);
		else
			count = unzReadCurrentFile(file->zipFile, ptr, remaining);

		if (count == 0){
			if (!tried){
				tried = true;
				continue;
			}

			Com_DPrintf(S_COLOR_RED "FS_Read: 0 bytes read from '%s'\n", file->name);

			if (fs_debug->integerValue)
				Com_Printf("FS_Read: %i bytes read from '%s'\n", size - remaining, file->name);

			return size - remaining;
		}

		if (count < 0)
			Com_Error(ERR_FATAL, "FS_Read: -1 bytes read from '%s'", file->name);

		ptr += count;
		remaining -= count;

		fs_readCount += count;
	}

	if (fs_debug->integerValue)
		Com_Printf("FS_Read: %i bytes read from '%s'\n", size, file->name);

	return size;
}

/*
 ==================
 FS_Write

 Properly handles partial writes
 ==================
*/
int FS_Write (fileHandle_t f, const void *buffer, int size){

	file_t	*file;
	byte	*ptr;
	int		remaining, count;
	bool	tried = false;

	if (!buffer)
		Com_Error(ERR_FATAL, "FS_Write: NULL buffer");
	if (size < 0)
		Com_Error(ERR_FATAL, "FS_Write: size < 0");

	file = FS_GetFileByHandle(f);

	if (file->mode != FS_WRITE && file->mode != FS_APPEND)
		Com_Error(ERR_FATAL, "FS_Write: cannot write to '%s'", file->name);

	ptr = (byte *)buffer;
	remaining = size;

	while (remaining){
		if (file->realFile)
			count = fwrite(ptr, 1, remaining, file->realFile);
		else
			Com_Error(ERR_FATAL, "FS_Write: cannot write to zipped file '%s'", file->name);

		if (count == 0){
			if (!tried){
				tried = true;
				continue;
			}

			Com_DPrintf(S_COLOR_RED "FS_Write: 0 bytes written to '%s'\n", file->name);

			if (fs_debug->integerValue)
				Com_Printf("FS_Write: %i bytes written to '%s'\n", size - remaining, file->name);

			return size - remaining;
		}

		if (count < 0)
			Com_Error(ERR_FATAL, "FS_Write: -1 bytes written to '%s'", file->name);

		ptr += count;
		remaining -= count;

		fs_writeCount += count;
	}

	if (fs_debug->integerValue)
		Com_Printf("FS_Write: %i bytes written to '%s'\n", size, file->name);

	return size;
}

/*
 ==================
 FS_Printf
 ==================
*/
int FS_Printf (fileHandle_t f, const char *fmt, ...){

	file_t	*file;
	va_list	argPtr;
	int		count;

	file = FS_GetFileByHandle(f);

	if (file->mode != FS_WRITE && file->mode != FS_APPEND)
		Com_Error(ERR_FATAL, "FS_Printf: cannot write to '%s'", file->name);

	if (file->realFile){
		va_start(argPtr, fmt);
		count = vfprintf(file->realFile, fmt, argPtr);
		va_end(argPtr);
	}
	else
		Com_Error(ERR_FATAL, "FS_Printf: cannot write to zipped file '%s'", file->name);

	if (count == 0){
		Com_DPrintf(S_COLOR_RED "FS_Printf: 0 chars written to '%s'\n", file->name);

		if (fs_debug->integerValue)
			Com_Printf("FS_Printf: 0 chars written to '%s'\n", file->name);

		return 0;
	}

	if (count < 0)
		Com_Error(ERR_FATAL, "FS_Printf: -1 chars written to '%s'", file->name);

	fs_writeCount += count;

	if (fs_debug->integerValue)
		Com_Printf("FS_Printf: %i chars written to '%s'\n", count, file->name);

	return count;
}

/*
 ==================
 FS_VPrintf
 ==================
*/
int FS_VPrintf (fileHandle_t f, const char *fmt, va_list argPtr){

	file_t	*file;
	int		count;

	file = FS_GetFileByHandle(f);

	if (file->mode != FS_WRITE && file->mode != FS_APPEND)
		Com_Error(ERR_FATAL, "FS_VPrintf: cannot write to '%s'", file->name);

	if (file->realFile)
		count = vfprintf(file->realFile, fmt, argPtr);
	else
		Com_Error(ERR_FATAL, "FS_VPrintf: cannot write to zipped file '%s'", file->name);

	if (count == 0){
		Com_DPrintf(S_COLOR_RED "FS_VPrintf: 0 chars written to '%s'\n", file->name);

		if (fs_debug->integerValue)
			Com_Printf("FS_VPrintf: 0 chars written to '%s'\n", file->name);

		return 0;
	}

	if (count < 0)
		Com_Error(ERR_FATAL, "FS_VPrintf: -1 chars written to '%s'", file->name);

	fs_writeCount += count;

	if (fs_debug->integerValue)
		Com_Printf("FS_VPrintf: %i chars written to '%s'\n", count, file->name);

	return count;
}

/*
 ==================
 FS_Length
 ==================
*/
int FS_Length (fileHandle_t f){

	file_t			*file;
	unz_file_info	info;
	int				cur, end;

	file = FS_GetFileByHandle(f);

	if (file->realFile){
		cur = ftell(file->realFile);
		fseek(file->realFile, 0, SEEK_END);
		end = ftell(file->realFile);
		fseek(file->realFile, cur, SEEK_SET);

		return end;
	}
	else {
		unzGetCurrentFileInfo(file->zipFile, &info, NULL, 0, NULL, 0, NULL, 0);

		return info.uncompressed_size;
	}
}

/*
 ==================
 FS_Tell
 ==================
*/
int FS_Tell (fileHandle_t f){

	file_t	*file;

	file = FS_GetFileByHandle(f);

	if (file->realFile)
		return ftell(file->realFile);
	else
		return unztell(file->zipFile);
}

/*
 ==================
 FS_Seek
 ==================
*/
void FS_Seek (fileHandle_t f, int offset, fsOrigin_t origin){

	file_t			*file;
	unz_file_info	info;
	byte			dummy[65536];
	int				remaining, count;
	bool			tried = false;

	file = FS_GetFileByHandle(f);

	if (file->realFile){
		switch (origin){
		case FS_SEEK_SET:
			fseek(file->realFile, offset, SEEK_SET);
			break;
		case FS_SEEK_CUR:
			fseek(file->realFile, offset, SEEK_CUR);
			break;
		case FS_SEEK_END:
			fseek(file->realFile, offset, SEEK_END);
			break;
		default:
			Com_Error(ERR_FATAL, "FS_Seek: bad origin for '%s'", file->name);
		}
	}
	else {
		unzGetCurrentFileInfo(file->zipFile, &info, NULL, 0, NULL, 0, NULL, 0);

		switch (origin){
		case FS_SEEK_SET:
			remaining = offset;
			break;
		case FS_SEEK_CUR:
			remaining = offset + unztell(file->zipFile);
			break;
		case FS_SEEK_END:
			remaining = offset + info.uncompressed_size;
			break;
		default:
			Com_Error(ERR_FATAL, "FS_Seek: bad origin for '%s'", file->name);
		}

		// Reopen the file
		unzCloseCurrentFile(file->zipFile);

		if (unzOpenCurrentFile(file->zipFile) != UNZ_OK)
			return;

		// Skip until the desired offset is reached
		if (remaining <= 0)
			return;

		if (remaining > info.uncompressed_size)
			remaining = info.uncompressed_size;

		while (remaining){
			count = remaining;
			if (count > sizeof(dummy))
				count = sizeof(dummy);

			count = unzReadCurrentFile(file->zipFile, dummy, count);

			if (count == 0){
				if (!tried){
					tried = true;
					continue;
				}

				break;
			}

			if (count < 0)
				break;

			remaining -= count;
		}
	}
}

/*
 ==================
 FS_Flush
 ==================
*/
void FS_Flush (fileHandle_t f){

	file_t	*file;

	file = FS_GetFileByHandle(f);

	if (file->realFile)
		fflush(file->realFile);
	else
		Com_Error(ERR_FATAL, "FS_Flush: cannot flush zipped file '%s'", file->name);
}

/*
 ==================
 FS_ForceFlush
 ==================
*/
void FS_ForceFlush (fileHandle_t f){

	file_t	*file;

	file = FS_GetFileByHandle(f);

	if (file->realFile)
		setvbuf(file->realFile, NULL, _IONBF, 0);
	else
		Com_Error(ERR_FATAL, "FS_ForceFlush: cannot flush zipped file '%s'", file->name);
}


/*
 ==============================================================================

 FUNCTIONS FOR ENTIRE FILES

 ==============================================================================
*/


/*
 ==================
 FS_OpenFileRead

 Returns file size or -1 if not found.
 Can open separate files as well as files inside pack files (both PAK 
 and PK3).
 ==================
*/
static int FS_OpenFileRead (const char *name, FILE **realFile, unzFile *zipFile){

	searchPath_t	*searchPath;
	pack_t			*pack;
	fileInPack_t	*fileInPack;
	char			path[MAX_OSPATH];
	uint			hashKey;

	// Get the hash key for the given file name
	hashKey = Str_HashKey(name, FILES_HASH_SIZE, false);

	// Search through the paths, one element at a time
	for (searchPath = fs_searchPaths; searchPath; searchPath = searchPath->next){
		if (searchPath->pack){
			// Search inside a pack file
			pack = searchPath->pack;

			for (fileInPack = pack->filesHashTable[hashKey]; fileInPack; fileInPack = fileInPack->nextHash){
				if (!FS_FileNameCompare(fileInPack->name, name)){
					// Found it!
					if (fs_debug->integerValue)
						Com_Printf("FS_OpenFileRead: '%s' (found in '%s')\n", name, pack->name);

					if (pack->pak){
						// PAK
						*realFile = fopen(pack->name, "rb");
						if (*realFile){
							fseek(*realFile, fileInPack->offset, SEEK_SET);

							return fileInPack->size;
						}
					}
					else if (pack->pk3){
						// PK3
						*zipFile = unzOpen(pack->name);
						if (*zipFile){
							if (unzLocateFile(*zipFile, name, 2) == UNZ_OK){
								if (unzOpenCurrentFile(*zipFile) == UNZ_OK)
									return fileInPack->size;
							}

							unzClose(*zipFile);
						}
					}

					Com_Error(ERR_FATAL, "Couldn't reopen %s", pack->name);
				}
			}
		}
		else {
			// Search in a directory tree
			Str_SPrintf(path, sizeof(path), "%s/%s", searchPath->path, name);

			*realFile = fopen(path, "rb");
			if (*realFile){
				// Found it!
				if (fs_debug->integerValue)
					Com_Printf("FS_OpenFileRead: %s (found in %s)\n", name, searchPath->path);

				return FS_FileLength(*realFile);
			}
		}
	}

	// Not found!
	if (fs_debug->integerValue)
		Com_Printf("FS_OpenFileRead: couldn't find %s\n", name);

	return -1;
}

/*
 ==================
 FS_OpenFileWrite

 Always returns 0 or -1 on error
 ==================
*/
static int FS_OpenFileWrite (const char *name, FILE **realFile){

	char	path[MAX_OSPATH];

	FS_BuildOSPath(fs_savePath->value, fs_game->value, name, path);

	// Create any needed subdirectories
	FS_CreateOSPath(path);

	// Open the file
	*realFile = fopen(path, "wb");
	if (*realFile){
		if (fs_debug->integerValue)
			Com_Printf("FS_OpenFileWrite: '%s' (opened in '%s%s%s')\n", name, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);

		return 0;
	}

	if (fs_debug->integerValue)
		Com_Printf("FS_OpenFileWrite: couldn't open '%s'\n", name);

	return -1;
}

/*
 ==================
 FS_OpenFileAppend

 Returns file size or -1 on error
 ==================
*/
static int FS_OpenFileAppend (const char *name, FILE **realFile){

	char	path[MAX_OSPATH];

	FS_BuildOSPath(fs_savePath->value, fs_game->value, name, path);

	// Create any needed subdirectories
	FS_CreateOSPath(path);

	// Open the file
	*realFile = fopen(path, "ab");
	if (*realFile){
		if (fs_debug->integerValue)
			Com_Printf("FS_OpenFileAppend: '%s' (opened in '%s%s%s')\n", name, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);

		return FS_FileLength(*realFile);
	}

	if (fs_debug->integerValue)
		Com_Printf("FS_OpenFileAppend: couldn't open '%s'\n", name);

	return -1;
}

/*
 ==================
 FS_OpenFile
 ==================
*/
int FS_OpenFile (const char *name, fsMode_t mode, fileHandle_t *f){

	file_t	*file;
	FILE	*realFile = NULL;
	unzFile	zipFile = NULL;
	int		size;

	// Try to open the file
	switch (mode){
	case FS_READ:
		size = FS_OpenFileRead(name, &realFile, &zipFile);
		break;
	case FS_WRITE:
		size = FS_OpenFileWrite(name, &realFile);
		break;
	case FS_APPEND:
		size = FS_OpenFileAppend(name, &realFile);
		break;
	default:
		Com_Error(ERR_FATAL, "FS_OpenFile: bad mode for '%s'", name);
	}

	if (size == -1){
		*f = 0;

		return -1;
	}

	// Create a new file handle
	file = FS_HandleForFile(f);

	Str_Copy(file->name, name, sizeof(file->name));
	file->mode = mode;
	file->realFile = realFile;
	file->zipFile = zipFile;

	return size;
}

/*
 ==================
 FS_CloseFile
 ==================
*/
void FS_CloseFile (fileHandle_t f){

	file_t	*file;

	file = FS_GetFileByHandle(f);

	if (file->realFile)
		fclose(file->realFile);
	else {
		unzCloseCurrentFile(file->zipFile);
		unzClose(file->zipFile);
	}

	Mem_Fill(file, 0, sizeof(file_t));
}

/*
 ==================
 FS_CopyFile
 ==================
*/
bool FS_CopyFile (const char *srcName, const char *dstName){

	char	srcPath[MAX_PATH_LENGTH], dstPath[MAX_PATH_LENGTH];
	FILE	*f1, *f2;
	byte	buffer[65536];
	int		remaining, count;

	// Make sure the file names are valid
	FS_BuildOSPath(fs_savePath->value, fs_game->value, srcName, srcPath);
	FS_BuildOSPath(fs_savePath->value, fs_game->value, dstName, dstPath);

	if (fs_debug->integerValue)
		Com_Printf("FS_CopyFile: '%s' to '%s' ('%s%s%s')\n", srcName, dstName, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);

	// Open the files
	f1 = fopen(srcPath, "rb");
	if (!f1){
		Com_DPrintf(S_COLOR_RED "FS_CopyFile: couldn't open '%s'\n", srcName);
		return false;
	}

	f2 = fopen(dstPath, "wb");
	if (!f2){
		fclose(f1);

		Com_DPrintf(S_COLOR_RED "FS_CopyFile: couldn't open '%s'\n", dstName);
		return false;
	}

	// Copy in small chunks
	remaining = FS_FileLength(f1);

	while (remaining){
		count = remaining;
		if (count > sizeof(buffer))
			count = sizeof(buffer);

		if (fread(buffer, 1, count, f1) != count){
			Com_DPrintf(S_COLOR_RED "FS_CopyFile: short read from '%s'\n", srcName);
			break;
		}

		if (fwrite(buffer, 1, count, f2) != count){
			Com_DPrintf(S_COLOR_RED "FS_CopyFile: short write to '%s'\n", dstName);
			break;
		}

		remaining -= count;
	}

	fclose(f1);
	fclose(f2);

	return (remaining == 0);
}

/*
 ==================
 FS_RenameFile
 ==================
*/
bool FS_RenameFile (const char *oldName, const char *newName){

	char	oldPath[MAX_PATH_LENGTH], newPath[MAX_PATH_LENGTH];

	FS_BuildOSPath(fs_savePath->value, fs_game->value, oldName, oldPath);
	FS_BuildOSPath(fs_savePath->value, fs_game->value, newName, newPath);

	if (fs_debug->integerValue)
		Com_Printf("FS_RenameFile: '%s' to '%s' ('%s%s%s')\n", oldName, newName, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);

	// Rename it
	if (rename(oldPath, newPath)){
		Com_DPrintf(S_COLOR_RED "FS_RenameFile: couldn't rename '%s' to '%s'\n", oldName, newName);
		return false;
	}

	return true;
}
		
/*
 ==================
 FS_RemoveFile
 ==================
*/
bool FS_RemoveFile (const char *name){

	char	path[MAX_PATH_LENGTH];

	FS_BuildOSPath(fs_savePath->value, fs_game->value, name, path);

	if (fs_debug->integerValue)
		Com_Printf("FS_RemoveFile: '%s' ('%s%s%s')\n", name, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);

	// Remove it
	if (remove(path)){
		Com_DPrintf(S_COLOR_RED "FS_RemoveFile: couldn't remove '%s'\n", name);
		return false;
	}

	return true;
}

/*
 ==================
 FS_FileExists
 ==================
*/
bool FS_FileExists (const char *name){

	fileHandle_t	f;

	FS_OpenFile(name, FS_READ, &f);
	if (!f)
		return false;

	FS_CloseFile(f);

	return true;
}

/*
 ==================
 FS_FindFile
 ==================
*/
findFile_t FS_FindFile (const char *name, char sourcePath[MAX_PATH_LENGTH]){

	searchPath_t	*searchPath;
	pack_t			*pack;
	fileInPack_t	*fileInPack;
	char			path[MAX_PATH_LENGTH];
	FILE			*f;
	uint			hashKey;

	// Get the hash key for the given file name
	hashKey = Str_HashKey(name, FILES_HASH_SIZE, false);

	// Search through the paths, one element at a time
	for (searchPath = fs_searchPaths; searchPath; searchPath = searchPath->next){
		if (searchPath->pack){
			// Search inside the pack file
			pack = searchPath->pack;

			for (fileInPack = pack->filesHashTable[hashKey]; fileInPack; fileInPack = fileInPack->nextHash){
				if (fileInPack->isDirectory)
					continue;

				if (!FS_FileNameCompare(fileInPack->name, name)){
					// Found it!
					if (fs_debug->integerValue)
						Com_Printf("FS_FindFile: '%s' (found in '%s')\n", name, pack->name);

					if (sourcePath)
						Str_Copy(sourcePath, pack->name, MAX_PATH_LENGTH);

					return FIND_PACK;
				}
			}
		}
		else {
			// Search in the directory tree
			Str_SPrintf(path, sizeof(path), "%s/%s", searchPath->path, name);

			f = fopen(path, "rb");
			if (f){
				// Found it!
				if (fs_debug->integerValue)
					Com_Printf("FS_FindFile: '%s' (found in '%s%s')\n", name, searchPath->path);

				fclose(f);

				if (sourcePath)
					Str_SPrintf(sourcePath, MAX_PATH_LENGTH, "%s", searchPath->path);

				return FIND_DIRECTORY;
			}
		}
	}

	// Not found!
	if (fs_debug->integerValue)
		Com_Printf("FS_FindFile: couldn't find '%s'\n", name);

	return FIND_NO;
}

/*
 ==================
 FS_ReadFile
 ==================
*/
int FS_ReadFile (const char *name, void **buffer){

	fileHandle_t	f;
	byte			*ptr;
	int				size;

	size = FS_OpenFile(name, FS_READ, &f);
	if (!f){
		if (buffer)
			*buffer = NULL;

		return -1;
	}

	if (!buffer){
		FS_CloseFile(f);
		return size;
	}

	*buffer = ptr = (byte *)Mem_ClearedAlloc(size + 1, TAG_TEMPORARY);

	FS_Read(f, ptr, size);
	FS_CloseFile(f);

	ptr[size] = 0;

	return size;
}

/*
 ==================
 FS_FreeFile
 ==================
*/
void FS_FreeFile (const void *buffer){

	if (!buffer)
		Com_Error(ERR_FATAL, "FS_FreeFile: NULL buffer");

	Mem_Free(buffer);
}

/*
 ==================
 FS_WriteFile
 ==================
*/
bool FS_WriteFile (const char *name, const void *buffer, int size){

	fileHandle_t	f;

	if (!buffer)
		Com_Error(ERR_FATAL, "FS_WriteFile: NULL buffer");

	FS_OpenFile(name, FS_WRITE, &f);
	if (!f)
		return false;

	FS_Write(f, buffer, size);
	FS_CloseFile(f);

	return true;
}


/*
 ==============================================================================

 DYNAMIC LIBRARY EXTRACTION

 ==============================================================================
*/


/*
 ==================
 FS_ExtractLibrary
 ==================
*/
bool FS_ExtractLibrary (const char *baseName, char libPath[MAX_PATH_LENGTH]){

	char		name[MAX_PATH_LENGTH];
	char		sourcePath[MAX_PATH_LENGTH];
	findFile_t	find;
	byte		*buffer;
	int			size;

	Str_SPrintf(name, sizeof(name), "%s%s%s", baseName, CPU_STRING, LIBRARY_EXTENSION);

	// Look for the library in the search paths
	find = FS_FindFile(name, sourcePath);

	if (find == FIND_NO)
		return false;

	if (find == FIND_DIRECTORY)
		Com_Printf("Found %s in '%s'\n", name, sourcePath);
	else
		Com_Printf("Found %s in pack '%s'\n", name, sourcePath);

	// Copy or extract the library to the current game directory
	size = FS_ReadFile(name, (void **)&buffer);
	if (!buffer)
		return false;

	if (!FS_WriteFile(name, buffer, size)){
		FS_FreeFile(buffer);
		return false;
	}

	FS_FreeFile(buffer);

	if (find == FIND_DIRECTORY)
		Com_Printf("Copied %s to '%s%s%s'\n", name, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);
	else
		Com_Printf("Extracted %s to '%s%s%s'\n", name, fs_savePath->value, PATH_SEPARATOR_STRING, fs_game->value);

	// Set the fully qualified path to the library
	FS_BuildOSPath(fs_savePath->value, fs_game->value, name, libPath);

	return true;
}


/*
 ==============================================================================

 FILE AND DIRECTORY SCANNING FUNCTIONS

 ==============================================================================
*/


/*
 ==================
 FS_SortFileList
 ==================
*/
static int FS_SortFileList (const void *elem1, const void *elem2){

	const char	*name1 = *(const char **)elem1;
	const char	*name2 = *(const char **)elem2;

	return Str_Compare(name1, name2);
}

/*
 ==================
 FS_AddFileToList
 ==================
*/
static int FS_AddFileToList (const char *name, const char **files, int fileCount){

	int		i;

	if (fileCount == MAX_LIST_FILES)
		return fileCount;	// Too many files

	// Ignore duplicates
	for (i = 0; i < fileCount; i++){
		if (!FS_FileNameCompare(files[i], name))
			return fileCount;	// Already in list
	}

	// Add it
	files[fileCount++] = Mem_DupString(name, TAG_TEMPORARY);

	return fileCount;
}

/*
 ==================
 FS_ListFiles

 TODO: when loading materials, fileInPack->name is q2e_pak0/materials when i should be only materials
 ==================
*/
const char **FS_ListFiles (const char *path, const char *extension, bool sort, int *numFiles){

	searchPath_t	*searchPath;
	pack_t			*pack;
	fileInPack_t	*fileInPack;
	char			name[MAX_PATH_LENGTH];
	const char		**sysFileList;
	int				sysNumFiles;
	const char		**fileList;
	const char		*files[MAX_LIST_FILES];
	int				fileCount = 0;
	int				i;

	// Search through the paths, one element at a time
	for (searchPath = fs_searchPaths; searchPath; searchPath = searchPath->next){
		if (searchPath->pack){
			// Search inside the pack file
			pack = searchPath->pack;

			for (i = 0, fileInPack = pack->files; i < pack->numFiles; i++, fileInPack++){
				// Check the path
				Str_ExtractFilePath(fileInPack->name, name, sizeof(name));

				if (FS_FileNameCompare(path, name))
					continue;

				// Check the extension
				Str_ExtractFileExtension(fileInPack->name, name, sizeof(name));

				if (fileInPack->isDirectory){
					if (extension == NULL || FS_FileNameCompare(extension, "/"))
						continue;
				}
				else {
					if (extension != NULL && FS_FileNameCompare(extension, name))
						continue;
				}

				// Extract the name
				Str_ExtractFileName(fileInPack->name, name, sizeof(name));

				// Add it
				fileCount = FS_AddFileToList(name, files, fileCount);
			}
		}
		else {
			// Search in the directory tree
			Str_SPrintf(name, sizeof(name), "%s/%s", searchPath->path, path);

			// Scan the directory
			sysFileList = Sys_ListFiles(name, extension, false, &sysNumFiles);

			// Add the files
			for (i = 0; i < sysNumFiles; i++)
				fileCount = FS_AddFileToList(sysFileList[i], files, fileCount);

			Sys_FreeFileList(sysFileList);
		}
	}

	if (!fileCount){
		*numFiles = 0;
		return NULL;
	}

	// Sort the list if desired
	if (sort)
		qsort(files, fileCount, sizeof(char *), FS_SortFileList);

	// Copy the list
	fileList = (const char **)Mem_ClearedAlloc((fileCount + 1) * sizeof(char *), TAG_TEMPORARY);

	for (i = 0; i < fileCount; i++)
		fileList[i] = files[i];

	fileList[i] = NULL;

	*numFiles = fileCount;

	return fileList;
}

/*
 ==================
 FS_ListFilteredFiles
 ==================
*/
const char **FS_ListFilteredFiles (const char *filter, bool sort, int *numFiles){

	searchPath_t	*searchPath;
	pack_t			*pack;
	fileInPack_t	*fileInPack;
	char			name[MAX_PATH_LENGTH];
	const char		**sysFileList;
	int				sysNumFiles;
	const char		**fileList;
	const char		*files[MAX_LIST_FILES];
	int				fileCount = 0;
	int				i;

	// Search through the paths, one element at a time
	for (searchPath = fs_searchPaths; searchPath; searchPath = searchPath->next){
		if (searchPath->pack){
			// Search inside the pack file
			pack = searchPath->pack;

			for (i = 0, fileInPack = pack->files; i < pack->numFiles; i++, fileInPack++){
				// Match filter
				if (!Str_MatchFilter(fileInPack->name, filter, false))
					continue;

				// Add it
				fileCount = FS_AddFileToList(fileInPack->name, files, fileCount);
			}
		}
		else {
			// Search in the directory tree
			Str_SPrintf(name, sizeof(name), "%s", searchPath->path);

			// Scan the directory
			sysFileList = Sys_ListFilteredFiles(name, filter, false, &sysNumFiles);

			// Add the files
			for (i = 0; i < sysNumFiles; i++)
				fileCount = FS_AddFileToList(sysFileList[i], files, fileCount);

			Sys_FreeFileList(sysFileList);
		}
	}

	if (!fileCount){
		*numFiles = 0;
		return NULL;
	}

	// Sort the list if desired
	if (sort)
		qsort(files, fileCount, sizeof(char *), FS_SortFileList);

	// Copy the list
	fileList = (const char **)Mem_ClearedAlloc((fileCount + 1) * sizeof(char *), TAG_TEMPORARY);

	for (i = 0; i < fileCount; i++)
		fileList[i] = files[i];

	fileList[i] = NULL;

	*numFiles = fileCount;

	return fileList;
}

/*
 ==================
 FS_FreeFileList
 ==================
*/
void FS_FreeFileList (const char **fileList){

	int		i;

	if (!fileList)
		return;

	for (i = 0; fileList[i]; i++)
		Mem_Free(fileList[i]);

	Mem_Free(fileList);
}


/*
 ==============================================================================

 MOD SCANNING FUNCTIONS

 ==============================================================================
*/


/*
 ==================
 FS_SortModList
 ==================
*/
static int FS_SortModList (const void *elem1, const void *elem2){

	const modList_t	*mod1 = *(const modList_t **)elem1;
	const modList_t	*mod2 = *(const modList_t **)elem2;

	return Str_Compare(mod1->description, mod2->description);
}

/*
 ==================
 FS_AddModToList
 ==================
*/
static int FS_AddModToList (const char *name, modList_t **mods, int modCount){

	FILE	*f;
	char	path[MAX_PATH_LENGTH];
	char	*description;
	int		length;
	int		i;

	if (modCount == MAX_LIST_MODS)
		return modCount;	// Too many mods

	// Ignore BASE_DIRECTORY
	if (!FS_FileNameCompare(name, BASE_DIRECTORY))
		return modCount;

	// Ignore duplicates
	for (i = 0; i < modCount; i++){
		if (!FS_FileNameCompare(mods[i]->directory, name))
			return modCount;	// Already in list
	}

	// Try to load a description file, otherwise use the directory name
	FS_BuildOSPath(fs_savePath->value, name, "description.txt", path);

	f = fopen(path, "rt");
	if (!f){
		FS_BuildOSPath(fs_basePath->value, name, "description.txt", path);

		f = fopen(path, "rt");
	}

	if (!f)
		description = Mem_DupString(name, TAG_TEMPORARY);
	else {
		length = FS_FileLength(f);

		description = (char *)Mem_ClearedAlloc(length + 1, TAG_TEMPORARY);

		fread(description, 1, length, f);
		description[length] = 0;

		fclose(f);
	}

	// Add it
	mods[modCount] = (modList_t *)Mem_ClearedAlloc(sizeof(modList_t), TAG_TEMPORARY);

	mods[modCount]->directory = Mem_DupString(name, TAG_TEMPORARY);
	mods[modCount]->description = description;

	modCount++;

	return modCount;
}

/*
 ==================
 FS_ListMods
 ==================
*/
modList_t **FS_ListMods (bool sort, int *numMods){

	const char	**sysFileList;
	int			sysNumFiles;
	modList_t	**modList;
	modList_t	*mods[MAX_LIST_MODS];
	int			modCount = 0;
	int			i;

	// Scan the save path
	sysFileList = Sys_ListFiles(fs_savePath->value, "/", false, &sysNumFiles);

	for (i = 0; i < sysNumFiles; i++)
		modCount = FS_AddModToList(sysFileList[i], mods, modCount);

	Sys_FreeFileList(sysFileList);

	// Scan the base path
	sysFileList = Sys_ListFiles(fs_basePath->value, "/", false, &sysNumFiles);

	for (i = 0; i < sysNumFiles; i++)
		modCount = FS_AddModToList(sysFileList[i], mods, modCount);

	Sys_FreeFileList(sysFileList);

	if (!modCount){
		*numMods = 0;
		return NULL;
	}

	// Sort the list if desired
	if (sort)
		qsort(mods, modCount, sizeof(modList_t *), FS_SortModList);

	// Copy the list
	modList = (modList_t **)Mem_ClearedAlloc((modCount + 1) * sizeof(modList_t *), TAG_TEMPORARY);

	for (i = 0; i < modCount; i++)
		modList[i] = mods[i];

	modList[i] = NULL;

	*numMods = modCount;

	return modList;
}

/*
 ==================
 FS_FreeModList
 ==================
*/
void FS_FreeModList (modList_t **modList){

	int		i;

	if (!modList)
		return;

	for (i = 0; modList[i]; i++){
		Mem_Free(modList[i]->directory);
		Mem_Free(modList[i]->description);

		Mem_Free(modList[i]);
	}

	Mem_Free(modList);
}


/*
 ==============================================================================

 I/O COUNTERS

 ==============================================================================
*/


/*
 ==================
 FS_AddToReadCount
 ==================
*/
void FS_AddToReadCount (int count){

	fs_readCount += count;
}

/*
 ==================
 FS_GetReadCount
 ==================
*/
int FS_GetReadCount (){

	return fs_readCount;
}

/*
 ==================
 FS_ResetReadCount
 ==================
*/
void FS_ResetReadCount (){

	fs_readCount = 0;
}

/*
 ==================
 FS_AddToWriteCount
 ==================
*/
void FS_AddToWriteCount (int count){

	fs_writeCount += count;
}

/*
 ==================
 FS_GetWriteCount
 ==================
*/
int FS_GetWriteCount (){

	return fs_writeCount;
}

/*
 ==================
 FS_ResetWriteCount
 ==================
*/
void FS_ResetWriteCount (){

	fs_writeCount = 0;
}


/*
 ==============================================================================

 PAK FORMAT LOADING

 ==============================================================================
*/


/*
 ==================
 FS_LoadPAK

 Takes an explicit (not game tree related) path to a pack file.

 Loads the header and directory, adding the files at the beginning of
 the list so they override previous pack files.
 ==================
*/
static pack_t *FS_LoadPAK (const char *packPath){

	pack_t			*pack;
	fileInPack_t	*fileInPack;
	FILE			*handle;
	pakHeader_t		header;
	pakFile_t		info;
	int				numFiles, i;
	uint			hashKey;

	// Open the file
	handle = fopen(packPath, "rb");
	if (!handle){
		Com_DPrintf(S_COLOR_RED "FS_LoadPAK: '%s' is not a zip file\n", packPath);
		return NULL;
	}

	// Parse the header
	fread(&header, 1, sizeof(pakHeader_t), handle);
	
	if (LittleLong(header.id) != PAK_ID){
		Com_DPrintf(S_COLOR_RED "FS_LoadPAK: '%s' has wrong header\n", packPath);

		fclose(handle);

		return NULL;
	}

	header.dirOfs = LittleLong(header.dirOfs);
	header.dirLen = LittleLong(header.dirLen);

	numFiles = header.dirLen / sizeof(pakFile_t);
	if (numFiles <= 0){
		Com_DPrintf(S_COLOR_RED "FS_LoadPAK: '%s' has no directory entries\n", packPath);

		fclose(handle);

		return NULL;
	}

	// Allocate the pack
	pack = (pack_t *)Mem_ClearedAlloc(sizeof(pack_t), TAG_COMMON);

	// Fill it in
	Str_Copy(pack->name, packPath, sizeof(pack->name));
	pack->pak = handle;
	pack->pk3 = NULL;
	pack->numFiles = numFiles;
	pack->files = fileInPack = (fileInPack_t *)Mem_ClearedAlloc(numFiles * sizeof(fileInPack_t), TAG_COMMON);
	Mem_Fill(pack->filesHashTable, 0, sizeof(pack->filesHashTable));

	// Parse the directory
	fseek(handle, header.dirOfs, SEEK_SET);

	for (i = 0; i < numFiles; i++){
		fread(&info, 1, sizeof(pakFile_t), handle);

		// Add the file
		Str_Copy(fileInPack->name, info.name, sizeof(fileInPack->name));
		fileInPack->size = LittleLong(info.fileLen);
		fileInPack->offset = LittleLong(info.filePos);
		fileInPack->isDirectory = false;

		// Add to hash table
		hashKey = Str_HashKey(fileInPack->name, FILES_HASH_SIZE, false);

		fileInPack->nextHash = pack->filesHashTable[hashKey];
		pack->filesHashTable[hashKey] = fileInPack;

		// Go to next the file
		fileInPack++;
	}

	return pack;
}


/*
 ==============================================================================

 PK3 FORMAT LOADING

 ==============================================================================
*/


/*
 ==================
 FS_LoadPK3

 Takes an explicit (not game tree related) path to a pack file.

 Loads the header and directory, adding the files at the beginning of
 the list so they override previous pack files.
 ==================
*/
static pack_t *FS_LoadPK3 (const char *packPath){

	pack_t			*pack;
	fileInPack_t	*fileInPack;
	unzFile			handle;
	unz_global_info	header;
	unz_file_info	info;
	char			name[MAX_QPATH];
	int				length;
	bool			isDirectory;
	int				i, numFiles;
	uint			hashKey;

	// Open the file
	handle = unzOpen(packPath);
	if (!handle){
		Com_DPrintf(S_COLOR_RED "FS_LoadPK3: '%s' is not a zip file\n", packPath);
		return NULL;
	}

	// Parse the header
	if (unzGetGlobalInfo(handle, &header) != UNZ_OK){
		Com_DPrintf(S_COLOR_RED "FS_LoadPK3: '%s' has wrong header\n", packPath);

		unzClose(handle);

		return NULL;
	}

	numFiles = header.number_entry;
	if (numFiles <= 0){
		Com_DPrintf(S_COLOR_RED "FS_LoadPK3: '%s' has no directory entries\n", packPath);

		unzClose(handle);

		return NULL;
	}

	// Allocate the pack
	pack = (pack_t *)Mem_ClearedAlloc(sizeof(pack_t), TAG_COMMON);

	// Fill it in
	Str_Copy(pack->name, packPath, sizeof(pack->name));
	pack->pak = NULL;
	pack->pk3 = handle;
	pack->numFiles = 0;
	pack->files = fileInPack = (fileInPack_t *)Mem_ClearedAlloc(numFiles * sizeof(fileInPack_t), TAG_COMMON);
	Mem_Fill(pack->filesHashTable, 0, sizeof(pack->filesHashTable));

	// Parse the directory
	unzGoToFirstFile(handle);

	for (i = 0; i < numFiles; i++){
		if (unzGetCurrentFileInfo(handle, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
			break;

		// Determine if it is a directory or a file
		length = Str_Length(name);
		if (name[length - 1] == '/'){
			name[length - 1] = 0;

			isDirectory = true;
		}
		else
			isDirectory = false;

		// Add the file
		Str_Copy(fileInPack->name, name, sizeof(fileInPack->name));
		fileInPack->isDirectory = isDirectory;
		fileInPack->size = info.uncompressed_size;
		fileInPack->offset = -1;

		// Add to hash table
		hashKey = Str_HashKey(fileInPack->name, FILES_HASH_SIZE, false);

		fileInPack->nextHash = pack->filesHashTable[hashKey];
		pack->filesHashTable[hashKey] = fileInPack;

		// Go to next the file
		unzGoToNextFile(handle);

		fileInPack++;
		pack->numFiles++;
	}

	return pack;
}


/*
 ==============================================================================

 DIRECTORY SETUP

 ==============================================================================
*/


/*
 ==================
 
 Sets fs_gameDirectory, adds the directory to the head of the path, then loads
 and adds all the pack files found (in alphabetical order).
 
 PK3 files are loaded later so they override PAK files.
 ==================
*/
static void FS_AddGameDirectory (const char *directory){

	searchPath_t	*searchPath;
	pack_t			*pack;
	char			name[MAX_OSPATH];
	const char		**fileList;
	int				numFiles;
	int				i;

	if (!directory || !directory[0])
		return;

	// Don't add the same directory twice
	for (searchPath = fs_searchPaths; searchPath; searchPath = searchPath->next){
		if (!FS_FileNameCompare(searchPath->path, directory))
			return;
	}

	Str_Copy(fs_gameDirectory, directory, sizeof(fs_gameDirectory));

	// Add any PAK files
	fileList = Sys_ListFiles(directory, ".pak", true, &numFiles);

	for (i = 0; i < numFiles; i++){
		Str_SPrintf(name, sizeof(name), "%s/%s", directory, fileList[i]);

		// Load it
		pack = FS_LoadPAK(name);
		if (!pack)
			continue;

		// Add the pack file to the search paths
		searchPath = (searchPath_t *)Mem_ClearedAlloc(sizeof(searchPath_t), TAG_COMMON);

		searchPath->pack = pack;
		searchPath->next = fs_searchPaths;
		fs_searchPaths = searchPath;
	}

	Sys_FreeFileList(fileList);

	// Add any PK3 files
	fileList = Sys_ListFiles(directory, ".pk3", true, &numFiles);

	for (i = 0; i < numFiles; i++){
		Str_SPrintf(name, sizeof(name), "%s/%s", directory, fileList[i]);

		// Load it
		pack = FS_LoadPK3(name);
		if (!pack)
			continue;

		// Add the pack file to the search paths
		searchPath = (searchPath_t *)Mem_ClearedAlloc(sizeof(searchPath_t), TAG_COMMON);

		searchPath->pack = pack;
		searchPath->next = fs_searchPaths;
		fs_searchPaths = searchPath;
	}

	Sys_FreeFileList(fileList);

	// Add the directory to the search paths
	searchPath = (searchPath_t *)Mem_ClearedAlloc(sizeof(searchPath_t), TAG_COMMON);

	Str_Copy(searchPath->path, directory, sizeof(searchPath->path));
	searchPath->next = fs_searchPaths;
	fs_searchPaths = searchPath;
}


/*
 ==============================================================================

 CONSOLE COMMANDS

 ==============================================================================
*/


/*
 ==================
 FS_ListFileHandles_f
 ==================
*/
static void FS_ListFileHandles_f (){

	file_t	*file;
	int		handles = 0;
	int		i;

	Com_Printf("Current active handles:\n");

	for (i = 0, file = fs_fileHandles; i < MAX_FILE_HANDLES; i++, file++){
		if (!file->realFile && !file->zipFile)
			continue;

		handles++;

		Com_Printf("%2i ", i + 1);

		switch (file->mode){
		case FS_READ:
			Com_Printf("(R) ");
			break;
		case FS_WRITE:
			Com_Printf("(W) ");
			break;
		case FS_APPEND:
			Com_Printf("(A) ");
			break;
		default:
			Com_Printf("(?) ");
			break;
		}

		Com_Printf(": %s\n", file->name);
	}

	Com_Printf("--------------------\n");
	Com_Printf("%i active handles\n", handles);
}

/*
 ==================
 FS_ListSearchPaths_f
 ==================
*/
static void FS_ListSearchPaths_f (){

	searchPath_t	*searchPath;
	pack_t			*pack;
	int				totalPacks = 0, totalFiles = 0;

	Com_Printf("Current search paths:\n");

	for (searchPath = fs_searchPaths; searchPath; searchPath = searchPath->next){
		if (searchPath->pack){
			pack = searchPath->pack;

			Com_Printf("%s (%i files)\n", pack->name, pack->numFiles);

			totalPacks++;
			totalFiles += pack->numFiles;
		}
		else
			Com_Printf("%s\n", searchPath->path);
	}

	Com_Printf("---------------------\n");
	Com_Printf("%i files in %i PAK/PK3 files\n", totalFiles, totalPacks);
}

/*
 ==================
 FS_ListFiles_f
 ==================
*/
static void FS_ListFiles_f (){

	const char	**fileList;
	int			numFiles;
	int			i;

	if (Cmd_Argc() < 2 || Cmd_Argc() > 3){
		Com_Printf("Usage: listFiles <directory> [extension]\n");
		return;
	}

	if (Cmd_Argc() == 2)
		fileList = FS_ListFiles(Cmd_Argv(1), NULL, true, &numFiles);
	else
		fileList = FS_ListFiles(Cmd_Argv(1), Cmd_Argv(2), true, &numFiles);

	for (i = 0; i < numFiles; i++)
		Com_Printf("%s\n", fileList[i]);

	FS_FreeFileList(fileList);

	if (numFiles == MAX_LIST_FILES)
		Com_Printf("...\n");

	Com_Printf("--------------------\n");
	Com_Printf("%i files listed\n", numFiles);
}

/*
 ==================
 FS_ListFilteredFiles_f
 ==================
*/
static void FS_ListFilteredFiles_f (){

	const char	**fileList;
	int			numFiles;
	int			i;

	if (Cmd_Argc() != 2){
		Com_Printf("Usage: listFilteredFiles <filter>\n");
		return;
	}

	fileList = FS_ListFilteredFiles(Cmd_Argv(1), true, &numFiles);

	for (i = 0; i < numFiles; i++)
		Com_Printf("%s\n", fileList[i]);

	FS_FreeFileList(fileList);

	if (numFiles == MAX_LIST_FILES)
		Com_Printf("...\n");

	Com_Printf("--------------------\n");
	Com_Printf("%i files listed\n", numFiles);
}

/*
 ==================
 FS_ListMods_f
 ==================
*/
static void FS_ListMods_f (){

	modList_t	**modList;
	int			numMods;
	int			i;

	modList = FS_ListMods(true, &numMods);

	for (i = 0; i < numMods; i++)
		Com_Printf("%-32s \"%s" S_COLOR_DEFAULT "\"\n", modList[i]->directory, modList[i]->description);

	FS_FreeModList(modList);

	if (numMods == MAX_LIST_MODS)
		Com_Printf("...\n");

	Com_Printf("--------------------\n");
	Com_Printf("%i mods listed\n", numMods);
}

/*
 ==================
 FS_TouchFile_f
 ==================
*/
static void FS_TouchFile_f (){

	fileHandle_t	f;

	if (Cmd_Argc() != 2){
		Com_Printf("Usage: touchFile <fileName>\n");
		return;
	}

	FS_OpenFile(Cmd_Argv(1), FS_READ, &f);
	if (!f){
		Com_Printf("Couldn't find %s\n", Cmd_Argv(1));
		return;
	}

	FS_CloseFile(f);

	Com_Printf("Touched %s\n", Cmd_Argv(1));
}


/*
 ==============================================================================

 STARTUP AND RESTART

 ==============================================================================
*/


/*
 ==================
 
 ==================
*/
static void FS_Startup (){

	// Add the directories
	FS_AddGameDirectory(fs_basePath->value);

	if (strstr(fs_baseGame->value, "..") || Str_FindChar(fs_baseGame->value, '.') || Str_FindChar(fs_baseGame->value, '/') || Str_FindChar(fs_baseGame->value, '\\') || Str_FindChar(fs_baseGame->value, ':') || !fs_baseGame->value[0] || !FS_FileNameCompare(fs_game->value, CODE_DIRECTORY) || !Str_ICompare(fs_game->value, DOCS_DIRECTORY) || !Str_ICompare(fs_game->value, SVN_DIRECTORY)){
		Com_Printf("Invalid game directory\n");
		CVar_SetString(fs_baseGame, BASE_DIRECTORY);
	}

	FS_AddGameDirectory(Str_VarArgs("%s/%s", fs_homePath->value, fs_baseGame->value));

	if (strstr(fs_game->value, "..") || Str_FindChar(fs_game->value, '.') || Str_FindChar(fs_game->value, '/') || Str_FindChar(fs_game->value, '\\') || Str_FindChar(fs_game->value, ':') || !fs_game->value[0] || !FS_FileNameCompare(fs_game->value, CODE_DIRECTORY) || !Str_ICompare(fs_game->value, DOCS_DIRECTORY) || !Str_ICompare(fs_game->value, SVN_DIRECTORY)){
		Com_Printf("Invalid game directory\n");
		CVar_SetString(fs_game, BASE_DIRECTORY);
	}

	FS_AddGameDirectory(Str_VarArgs("%s/%s", fs_homePath->value, fs_game->value));

	// Set the current game
	Str_Copy(fs_curGame, fs_game->value, sizeof(fs_curGame));

	// Print current search paths
	FS_ListSearchPaths_f();
}

/*
 ==================
 
 ==================
*/
void FS_Restart (){

	// Check if the current game directory needs to be changed
	if (!FS_FileNameCompare(fs_curGame, fs_game->value)){
		FS_Startup();
		return;
	}

	// Shutdown
	FS_Shutdown();

	// Initialize
	FS_Init();
}


/*
 ==============================================================================

 INITIALIZATION AND SHUTDOWN

 ==============================================================================
*/


/*
 ==================
 
 ==================
*/
void FS_Init (){

	Com_Printf("-------- File System Initialization --------\n");

	// Register variables
	fs_debug = CVar_Register("fs_debug", "0", CVAR_BOOL, 0, "Print debugging information", 0, 0);
	fs_savePath = CVar_Register("fs_savePath", Sys_DefaultSaveDirectory(), CVAR_STRING, CVAR_INIT, "Save path", 0, 0);
	fs_game = CVar_Register("fs_game", BASE_DIRECTORY, CVAR_STRING, CVAR_SERVERINFO | CVAR_INIT, "Current game directory", 0, 0);

	fs_homePath = CVar_Register("fs_homePath", Sys_CurrentDirectory(), CVAR_STRING,  CVAR_INIT, NULL, 0, 0);
	fs_basePath = CVar_Register("fs_basePath", Str_VarArgs("%s/%s", Sys_CurrentDirectory(), BASE_DIRECTORY), CVAR_STRING,  CVAR_INIT, NULL, 0, 0);
	fs_baseGame = CVar_Register("fs_baseGame", BASE_DIRECTORY, CVAR_STRING, CVAR_INIT, NULL, 0, 0);

	// Add commands
	Cmd_AddCommand("listFileHandles", FS_ListFileHandles_f, "Lists active file handles", NULL);
	Cmd_AddCommand("listSearchPaths", FS_ListSearchPaths_f, "Lists current search paths", NULL);
	Cmd_AddCommand("listFiles", FS_ListFiles_f, "Lists files in a directory with an optional extension", NULL);
	Cmd_AddCommand("listFilteredFiles", FS_ListFilteredFiles_f, "Lists files matching a filter", NULL);
	Cmd_AddCommand("listMods", FS_ListMods_f, "Lists available game mods", NULL);
	Cmd_AddCommand("touchFile", FS_TouchFile_f, "Touches a file", NULL);

	// Add the directories
	FS_Startup();

	// If we can't find default.cfg, assume that the paths are busted and error
	// out now, rather than getting an unreadable graphics screen when the main
	// menu UI fails to load
	if (FS_ReadFile("default.cfg", NULL) == -1)
		Com_Error(ERR_FATAL, "Couldn't find default.cfg - Check your " ENGINE_NAME " installation");

	// Execute config files
	Cmd_AppendText("exec default.cfg\n");
	Cmd_AppendText("exec q2econfig.cfg\n");
	Cmd_AppendText("exec autoexec.cfg\n");

	Cmd_ExecuteBuffer();

	Com_Printf("--------------------------------------------\n");
}

/*
 ==================
 
 ==================
*/
void FS_Shutdown (){

	file_t			*handle;
	searchPath_t	*next;
	pack_t			*pack;
	int				i;

	// Remove commands
	Cmd_RemoveCommand("listFileHandles");
	Cmd_RemoveCommand("listSearchPaths");
	Cmd_RemoveCommand("listFiles");
	Cmd_RemoveCommand("listFilteredFiles");
	Cmd_RemoveCommand("listMods");
	Cmd_RemoveCommand("touchFile");

	// Close all files
	for (i = 0, handle = fs_fileHandles; i < MAX_FILE_HANDLES; i++, handle++){
		if (!handle->realFile && !handle->zipFile)
			continue;

		if (handle->realFile)
			fclose(handle->realFile);
		else if (handle->zipFile){
			unzCloseCurrentFile(handle->zipFile);
			unzClose(handle->zipFile);
		}

		Mem_Fill(handle, 0, sizeof(*handle));
	}

	// Free search paths
	while (fs_searchPaths){
		if (fs_searchPaths->pack){
			pack = fs_searchPaths->pack;

			if (pack->pak)
				fclose(pack->pak);
			else if (pack->pk3)
				unzClose(pack->pk3);

			Mem_Free(pack->files);
			Mem_Free(pack);
		}

		next = fs_searchPaths->next;
		Mem_Free(fs_searchPaths);
		fs_searchPaths = next;
	}

	// Reset I/O counters
	fs_readCount = 0;
	fs_writeCount = 0;
}