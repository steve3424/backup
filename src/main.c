/*
- Recursively searches the specified source directory
- Creates each directory inside backup directory if it doesn't exist
- Files are only copied if the write time differs by 10 seconds
- According to this: 
	https://docs.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime 
  NT FAT only has a resolution of 2 seconds so the diff must be at least 2,
  I used 10 just to be safe.
- It also has a fallback check in case the files differ, but it can't copy for
  some reason, e.g. 'access denied'. It will compare the files byte by byte
  to see if they REALLY are different and only consider it an error if they 
  differ.
- Since the current log is not finished writing an incomplete copy will be
  backed up, but a full copy is backed up the next time it is run
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>
#include <shlobj.h>
#include "win32_timing.h"

static const char* global_log_dir = "..\\logs";
static const char* global_default_file = "..\\default.txt";

static char global_src_buffer[1024*1024];
static char global_dst_buffer[1024*1024];

typedef struct Log {
	HANDLE handle;
	int files_checked_count;
	int folders_checked_count;
	int should_copy_count;
	int copy_success_count;
	int error_count;
} Log;

static inline void Log_WriteMessage(Log* log, const char* string, ...) {
	va_list args;
	va_start(args, string);
	
	char buffer[1024];	
	_vsnprintf_s(buffer, sizeof(buffer),
			     _TRUNCATE, string, args);

	DWORD bytes_to_write = (DWORD)strlen(buffer);
	DWORD bytes_written = 0;
	WriteFile(log->handle,
		      buffer,
		      bytes_to_write,
		      &bytes_written,
		      NULL);
}

typedef struct Path {
	char path[MAX_PATH];
	int top;
} Path;

static inline void Path_PushSourceToDestination(const Path* src, 
					   				                  Path* dst) 
{
	// find backslash as in source to start pushing
	// this dir to destination
	int i = src->top - 1;
	while((src->path[i] != '\\') && (0 < i)) {
		i--;
	}

	// push source dir to dst path
	// leave one char for null term in dst
	while((i < src->top) && (dst->top < MAX_PATH)) {
		dst->path[dst->top] = src->path[i];
		i++;
		dst->top++;
	}
}

static inline void Path_PushToPath(Path* path, const char* to_push) {
	int i = 0;
	while((to_push[i] != '\0') && (path->top < MAX_PATH)) {
		path->path[path->top] = to_push[i];
		i++;
		path->top++;
	}
}

static inline void Path_PopFullDir(Path* path) {
	path->top--;
	while((path->path[path->top] != '\\') && (0 < path->top)) {
		path->path[path->top] = '\0';
		path->top--;
	}
	path->path[path->top] = '\0';
}

static inline void Path_PopLastName(Path* path) {
	path->top--;
	while((path->path[path->top] != '\\') && (0 < path->top)) {
		path->path[path->top] = '\0';
		path->top--;
	}
	path->top++;
}

static bool ShouldCopy(const char* src, const char* dst) {
	// NOTE: This assumes that if GetFileAttributesExA is called
	//       with a dst path that doesn't exist, nothing will be
	//       copied into dst_file_data. If nothing is copied, then
	//       dst_file_date.ftLastWriteTime will be 0. This means
	//       that the comparison will always be greater than 10,
	//       correctly returning that the file should be copied.
	//       I don't know if this is strictly defined, but it seems
	//       very reasonable that is the case.
	ULARGE_INTEGER src_ft = {0};
	WIN32_FILE_ATTRIBUTE_DATA src_file_data = {0};
	GetFileAttributesExA(src, GetFileExInfoStandard, &src_file_data);
	memcpy(&src_ft, 
		   &src_file_data.ftLastWriteTime, 
		   sizeof(src_file_data.ftLastWriteTime));

	ULARGE_INTEGER dst_ft = {0};
	WIN32_FILE_ATTRIBUTE_DATA dst_file_data = {0};
	GetFileAttributesExA(dst, GetFileExInfoStandard, &dst_file_data);
	memcpy(&dst_ft, 
		   &dst_file_data.ftLastWriteTime, 
		   sizeof(dst_file_data.ftLastWriteTime));

	ULONGLONG ten_seconds = 10 * (ULONGLONG)10000000;
	ULONGLONG diff = src_ft.QuadPart > dst_ft.QuadPart ? 
			         src_ft.QuadPart - dst_ft.QuadPart : 
			         dst_ft.QuadPart - src_ft.QuadPart;

	if(diff > ten_seconds) {
		return true;
	}
	return false;
}

static bool FileContentsAreTheSame(const char* src, const char* dst) {
	HANDLE src_file = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, 
					              0, OPEN_EXISTING, 0, 0);
	HANDLE dst_file = CreateFileA(dst, GENERIC_READ, FILE_SHARE_READ, 
					              0, OPEN_EXISTING, 0, 0);

	// NOTE: Returning false here will cause the overall backup to consider
	//       this an error. Maybe there is some more checking to do here.
	if (src_file == INVALID_HANDLE_VALUE || dst_file == INVALID_HANDLE_VALUE) {
		return false;
	}

	// Check file sizes for a fast fail
	LARGE_INTEGER src_file_size;
	LARGE_INTEGER dst_file_size;
	GetFileSizeEx(src_file, &src_file_size);
	GetFileSizeEx(dst_file, &dst_file_size);
	if(src_file_size.QuadPart != dst_file_size.QuadPart) {
		return false;
	}


	// Compare files byte by byte
	DWORD src_bytes_read;
	DWORD dst_bytes_read;
	do {
		ReadFile(src_file, global_src_buffer, 
				 (DWORD)sizeof(global_src_buffer), 
				 &src_bytes_read, 0);
		ReadFile(dst_file, global_dst_buffer, 
				 (DWORD)sizeof(global_dst_buffer), 
				 &dst_bytes_read, 0);

		if(src_bytes_read != dst_bytes_read) {
			// NOTE: This would probably never get called
			//       if we are checking for file sizes
			//       above.
			return false;
		}
		else {
			int res = memcmp(global_src_buffer, global_dst_buffer, src_bytes_read);
			if(res != 0) {
				return false;
			}
		}

	} while(0 < src_bytes_read && 0 < dst_bytes_read);

	return true;
}

static void BackupDirectoryRecursively(Path* src, 
				                       Path* dst,
				                       Log* log)
{
	BOOL dir_created = CreateDirectoryA(dst->path, NULL);
	DWORD last_error = GetLastError();
	if(last_error == ERROR_PATH_NOT_FOUND) {
		log->error_count++;
		Log_WriteMessage(log, "[ERROR] Could not create dir "
				              "\'%s\'"
							  "This folder and sub folders will not be backed up\r\n",
							  dst->path);

		Path_PopFullDir(src);
		Path_PopFullDir(dst);

		return;
	}

	// Asterisk is used in src->path to get all files/dirs.
	// In dst->path, it serves as a sentinel for the recursive pattern.
	Path_PushToPath(src, "\\*");
	Path_PushToPath(dst, "\\*");

	WIN32_FIND_DATA file_data;
	HANDLE file_handle = FindFirstFileA(src->path, &file_data);
	if(file_handle == INVALID_HANDLE_VALUE) {
		log->error_count++;
		Log_WriteMessage(log, "[ERROR] Could not find file in folder "
				              "\'%s\'"
							  "This folder/sub-folders and all files will not be backed up\r\n",
							  src->path);

		Path_PopFullDir(src);
		Path_PopFullDir(dst);

		return;
	}

	BOOL found_next;
	do {

		bool is_directory = (file_data.dwFileAttributes & 
				             FILE_ATTRIBUTE_DIRECTORY);
		bool is_this_dir = !strcmp(".", file_data.cFileName);
		bool is_parent_dir = !strcmp("..", file_data.cFileName);
		if(is_this_dir || is_parent_dir) {
			// do nothing
		}
		else {
			// Pop here in case there is a file name 
			// that was pushed.
			// The sentinel '*' is added at the top
			// so this works when no file name was
			// pushed yet.
			Path_PopLastName(src);
			Path_PushToPath(src, file_data.cFileName);
			Path_PopLastName(dst);
			Path_PushToPath(dst, file_data.cFileName);
		
			if(is_directory) {
				BackupDirectoryRecursively(src, dst, log);
			}
			else {
				bool should_copy = ShouldCopy(src->path, dst->path);
				if(should_copy) {
					log->should_copy_count++;

					BOOL file_copied = CopyFileA(src->path, dst->path, false);
					DWORD last_error = GetLastError();
					if(!file_copied) {
						// CopyFileA failed for some reason. Use backup method
						// of byte comparison to determine if it REALLY needs
						// to be copied.
						bool files_are_the_same = FileContentsAreTheSame(src->path, dst->path);
						if(!files_are_the_same) {
							char* buffer = NULL;
							DWORD message = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, 
														  0, last_error, 0, buffer, 256, 0);
							Log_WriteMessage(log, "[ERROR] %s\r\n"
									              "[PATH] \'%s\'"
												  "Was not copied.\r\n",
												  buffer,
												  src->path);

							log->error_count++;
						}
						else {
							log->should_copy_count--;
						}
					}
					else {
						log->copy_success_count++;
						printf("Copied %s\n", src->path);
					}
				}

				log->files_checked_count++;
			}
		}

		found_next = FindNextFileA(file_handle, &file_data);

	} while(found_next);

	FindClose(file_handle);

	log->folders_checked_count++;

	Path_PopFullDir(src);
	Path_PopFullDir(dst);
}

static inline void OpenLogFile(Log* log) {
	CreateDirectoryA(global_log_dir, NULL);

	char log_file_path[128] = {0};
	SYSTEMTIME system_time = {0};
	GetLocalTime(&system_time);
	_snprintf_s(log_file_path, sizeof(log_file_path), _TRUNCATE, 
		    "%s\\log_%d-%d-%d_%d-%d-%d.txt", 
			global_log_dir,
		    system_time.wMonth,
		    system_time.wDay,
		    system_time.wYear,
		    system_time.wHour,
		    system_time.wMinute,
		    system_time.wSecond);

	log->handle = CreateFileA(log_file_path,
				  GENERIC_READ | GENERIC_WRITE,
				  FILE_SHARE_READ,
				  NULL, CREATE_NEW,
				  FILE_ATTRIBUTE_NORMAL,
				  NULL);
}

static int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData)
{

    if(uMsg == BFFM_VALIDATEFAILED)
    {
		printf("Validate failed. %s\n", (char*)lParam);
		return 1;
    }

    return 0;
}

// TODO: Handle shortcut paths properly
static void SetSourcePath(Path* src) {
    BROWSEINFO bi = {0};
    bi.lpszTitle  = "Choose folder to backup...";
    bi.ulFlags    = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn       = BrowseCallbackProc;

    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
	// Not sure if below adds null term
	SHGetPathFromIDListA(pidl, src->path);

	src->top = strlen(src->path);

	ILFree(pidl);
}

static void SetDestinationPath(Path* dst) {
    BROWSEINFO bi = {0};
    bi.lpszTitle  = "Choose backup destination...";
    bi.ulFlags    = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn       = BrowseCallbackProc;

    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
	// Not sure if below adds null term
	SHGetPathFromIDListA(pidl, dst->path);

	dst->top = strlen(dst->path);

	ILFree(pidl);
}

typedef struct {
	int file_size;
	void* file;
} Win32FileResult;

static void Win32FreeFileResult(Win32FileResult* file_result) {
	file_result->file_size = 0;
	if (file_result->file) {
		VirtualFree(file_result->file, 0, MEM_RELEASE);
	}
}

static Win32FileResult Win32ReadEntireFile(const char* file_name) {
	Win32FileResult result = {0};

	HANDLE file_handle = CreateFileA(file_name, GENERIC_READ, 
			                         FILE_SHARE_READ, 0, 
									 OPEN_EXISTING, 0, 0);

	if (file_handle != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER file_size;
		if (GetFileSizeEx(file_handle, &file_size)) {
			uint32_t file_size32 = (uint32_t)file_size.QuadPart; 
			result.file = VirtualAlloc(0, file_size32, 
						       MEM_RESERVE|MEM_COMMIT,
						       PAGE_READWRITE);
			if (result.file) {
				DWORD bytes_read;
				if (ReadFile(file_handle, result.file, 
					     (DWORD)file_size.QuadPart, 
					     &bytes_read, 0) && 
					     (file_size32 == bytes_read)) {
					// SUCCESS !!
					result.file_size = file_size32;
				}
				else {
					Win32FreeFileResult(&result);
					result.file = NULL;
				}
			}
			else {
				// VirtualAlloc() error
			}
		}
		else {
			// GetFileSize() error
		}

		CloseHandle(file_handle);
	}
	else {
		// CreateFile() error
	}

	return result;
}

// TODO: Make sure this doesn't overflow the buffers on long paths.
//       Make sure it is always successful on correct paths.
//       Other than that it can fail.
static bool LoadDefaultPaths(Path* src, Path* dst) {
	src->top = 0;
	dst->top = 0;

	Win32FileResult default_file = Win32ReadEntireFile(global_default_file);
	char* file = (char*)default_file.file;
	if(file) {
		int file_read_i = 0;
		while((file_read_i < default_file.file_size) &&
			  (src->top < MAX_PATH - 1) &&
			  (file[file_read_i] != ','))
		{
			src->path[src->top++] = file[file_read_i++];
		}

		// move past comma
		++file_read_i;
		src->path[src->top] = '\0';

		while((file_read_i < default_file.file_size) &&
			  (dst->top < MAX_PATH - 1) &&
			  ((file[file_read_i] != '\r') && (file[file_read_i] != '\n')))
		{
			dst->path[dst->top++] = file[file_read_i++];
		}

		dst->path[dst->top] = '\0';

	}

	Win32FreeFileResult(&default_file);

	return PathFileExistsA(src->path) && PathFileExistsA(dst->path);
}

int main() {
	Path src = {0};
	Path dst = {0};

	if(!LoadDefaultPaths(&src, &dst)) {
		// TODO: Change dialog strings to include selections.
		SetSourcePath(&src);
		if(src.top == 0) {
			MessageBox(NULL,
				       "Invalid source folder.\n"
				       "Backup not started.",
				       NULL, MB_OK);
			return 1;
		}

		SetDestinationPath(&dst);
		if(dst.top == 0) {
			MessageBox(NULL,
				       "Invalid destination folder.\n"
				       "Backup not started.",
				       NULL, MB_OK);
			return 1;
		}
	}

	// TODO: Maybe don't fail on bad log file, but just
	//       give message box with specific error.
	Log log = {0};
	OpenLogFile(&log);
	if(log.handle == INVALID_HANDLE_VALUE) {
		MessageBox(NULL,
		   	       "Log couldn't be created.\n"
			       "Backup not started.",
		   	       NULL, MB_OK);
		return 1;
	}

	// TODO: confirmation box before starting

	Path_PushSourceToDestination(&src, &dst);

	// LOG START TIME
	SYSTEMTIME system_time_start;
	GetLocalTime(&system_time_start);
	Log_WriteMessage(&log, "[BACKUP START] Backup started on %d-%d-%d at %d:%d:%d\r\n",
		                   system_time_start.wMonth,
		                   system_time_start.wDay,
		                   system_time_start.wYear,
		                   system_time_start.wHour,
		                   system_time_start.wMinute,
		                   system_time_start.wSecond);

	Win32StartTimer();
	BackupDirectoryRecursively(&src, &dst, &log);
	Win32StopTimer();
	double seconds_elapsed = Win32GetSecondsElapsed();

	// LOG STATS
	Log_WriteMessage(&log, "[STATS]\r\n",
                     "\t%d files checked\r\n"
                     "\t%d folders checked\r\n"
                     "\t%d out of %d files copied.\r\n"
                     "\t%d errors occurred.\r\n",
		             log.files_checked_count,
		             log.folders_checked_count,
		             log.copy_success_count,
		             log.should_copy_count,
		             log.error_count);

	// LOG END TIME
	SYSTEMTIME system_time_end;
	GetLocalTime(&system_time_end);
	Log_WriteMessage(&log, "[END] Backup ended on %d-%d-%d at %d:%d:%d\r\n", 
		                   system_time_end.wMonth,
		                   system_time_end.wDay,
		                   system_time_end.wYear,
		                   system_time_end.wHour,
		                   system_time_end.wMinute,
		                   system_time_end.wSecond);

	ULARGE_INTEGER total_bytes = {0};
	ULARGE_INTEGER free_bytes = {0};
	GetDiskFreeSpaceExA(dst.path, NULL, &total_bytes, &free_bytes);
	total_bytes.QuadPart /= 1024ull * 1024ull * 1024ull;
	free_bytes.QuadPart /= 1024ull * 1024ull * 1024ull;

	Log_WriteMessage(&log, "[BACKUP_END] Backup Complete!!\r\n"
			               "\t\tTime elapsed: %.3f seconds\r\n"
		                   "\t\t%d files checked\r\n"
		                   "\t\t%d folders checked\r\n"
		                   "\t\t%d out of %d files copied.\r\n"
		                   "\t\t%d errors occurred.\r\n"
		                   "\t\t%lld free GB\r\n"
		                   "\t\t%lld total GB\r\n"
						   "\r\n\r\n",
			               seconds_elapsed,
		                   log.files_checked_count,
		                   log.folders_checked_count,
		                   log.copy_success_count,
		                   log.should_copy_count,
		                   log.error_count,
		                   free_bytes.QuadPart,
		                   total_bytes.QuadPart);

	char message_stats_string[256] = {0};
	_snprintf_s(message_stats_string, sizeof(message_stats_string), _TRUNCATE,
		    "Backup Complete!!\r\n"
			"Time elapsed: %.3f seconds\r\n"
		    "%d files checked\r\n"
		    "%d folders checked\r\n"
		    "%d out of %d files copied.\r\n"
		    "%d errors occurred.\r\n"
		    "%lld free GB\r\n"
		    "%lld total GB\r\n",
			seconds_elapsed,
		    log.files_checked_count,
		    log.folders_checked_count,
		    log.copy_success_count,
		    log.should_copy_count,
		    log.error_count,
		    free_bytes.QuadPart,
		    total_bytes.QuadPart);
	
	MessageBox(NULL, message_stats_string, "Complete", MB_OK);
	
	return 0;
}
