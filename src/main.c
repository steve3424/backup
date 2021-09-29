/*
- Recursively searches the specified source directory
- Creates each directory inside backup directory if it doesn't exist
- Path names are treated like a stack where the folders are pushed
  and popped as it searches the directory structure
- Files are only copied if the write time differs by 10 seconds
- According to this: 
	https://docs.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime 
  NT FAT only has a resolution of 2 seconds so the diff must be at least 2,
  I used 10 just to be safe.
- Since the current log is not finished writing an incomplete copy will be
  backed up, but a full copy is backed up the next time it is run
*/


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>
#include <shlobj.h>

typedef struct Path {
	char path[MAX_PATH];
	int top;
} Path;

typedef struct LogStruct {
	HANDLE handle;
	int files_checked_count;
	int folders_checked_count;
	int should_copy_count;
	int copy_success_count;
	int error_count;
} LogStruct;

static inline void LogMessage(LogStruct* log, const char* message) {
	DWORD bytes_to_write = (DWORD)strlen(message);
	DWORD bytes_written = 0;
	WriteFile(log->handle,
		  message,
		  bytes_to_write,
		  &bytes_written,
		  NULL);
}

static inline void PushSourceToDestination(const Path* source, 
					   				       Path* dest) 
{
	int i = source->top - 1;
	while((source->path[i] != '\\') && (0 < i)) {
		i--;
	}

	while((i < source->top) && (dest->top < MAX_PATH)) {
		dest->path[dest->top] = source->path[i];
		i++;
		dest->top++;
	}
}

static inline void PushToPath(Path* path, const char* to_push) {
	int i = 0;
	while((to_push[i] != '\0') && (path->top < MAX_PATH)) {
		path->path[path->top] = to_push[i];
		i++;
		path->top++;
	}
}

static inline void PopFullDir(Path* path) {
	path->top--;
	while((path->path[path->top] != '\\') && (0 < path->top)) {
		path->path[path->top] = '\0';
		path->top--;
	}
	path->path[path->top] = '\0';
}

static inline void PopLastName(Path* path) {
	path->top--;
	while((path->path[path->top] != '\\') && (0 < path->top)) {
		path->path[path->top] = '\0';
		path->top--;
	}
	path->top++;
}

static bool CheckTimeDiff(const char* source, const char* destination) {
	ULARGE_INTEGER source_ft = {0};
	WIN32_FILE_ATTRIBUTE_DATA source_file_data = {0};
	GetFileAttributesExA(source, GetFileExInfoStandard, &source_file_data);
	memcpy(&source_ft, 
		&source_file_data.ftLastWriteTime, 
		sizeof(source_file_data.ftLastWriteTime));

	ULARGE_INTEGER destination_ft = {0};
	WIN32_FILE_ATTRIBUTE_DATA destination_file_data = {0};
	GetFileAttributesExA(destination,
			     GetFileExInfoStandard,
			     &destination_file_data);
	memcpy(&destination_ft, 
		&destination_file_data.ftLastWriteTime, 
		sizeof(destination_file_data.ftLastWriteTime));

	ULONGLONG ten_seconds = 10 * (ULONGLONG)10000000;
	ULONGLONG diff = source_ft.QuadPart > destination_ft.QuadPart ? 
			 source_ft.QuadPart - destination_ft.QuadPart : 
			 destination_ft.QuadPart - source_ft.QuadPart;

	if(diff > ten_seconds) {
		return true;
	}
	return false;
}

static void BackupDirectoryRecursively(Path* source, 
				       Path* destination,
				       LogStruct* log)
{

	CreateDirectoryA(destination->path, NULL);
	PushToPath(destination, "\\*");
	PushToPath(source, "\\*");

	WIN32_FIND_DATA file_data;
	HANDLE file_handle = FindFirstFileA(source->path, &file_data);
	if(file_handle == INVALID_HANDLE_VALUE) {
		log->error_count++;
		LogMessage(log, "\tERROR: INVALID_HANDLE_VALUE\r\n\t");
		LogMessage(log, source->path);
		LogMessage(log, "\r\n\tThis folder will not be backed up\r\n");

		PopFullDir(source);
		PopFullDir(destination);

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
		else if(!is_directory) {
			log->files_checked_count++;

			PopLastName(source);
			PushToPath(source, file_data.cFileName);

			PopLastName(destination);
			PushToPath(destination, file_data.cFileName);

			bool should_copy = CheckTimeDiff(source->path, 
							 destination->path);
			if(should_copy) {
				log->should_copy_count++;

				if(CopyFileA(source->path, destination->path, false) != 
					     0) 
				{
					printf("copied %s\n", source->path);
					log->copy_success_count++;
				}
				else {
					DWORD last_error = GetLastError();
					char message_stats_string[256] = {0};
					_snprintf_s(message_stats_string, 256, _TRUNCATE,
						    "\tERROR: Copy failed with error code %d\r\n",
						    last_error);
					LogMessage(log, "\tERROR: ");
					LogMessage(log, source->path);
					LogMessage(log, " was not copied\r\n");
					LogMessage(log, message_stats_string);

					log->error_count++;
				}
			}
		}
		else {
			log->folders_checked_count++;

			PopLastName(source);
			PushToPath(source, file_data.cFileName);

			PopLastName(destination);
			PushToPath(destination, file_data.cFileName);

			BackupDirectoryRecursively(source, destination, log);
		}

		found_next = FindNextFileA(file_handle, &file_data);

	} while(found_next);

	FindClose(file_handle);

	PopFullDir(source);
	PopFullDir(destination);
}

static inline void OpenLogFile(LogStruct* log) {
	CreateDirectoryA("..\\logs", NULL);

	char log_file_path[MAX_PATH] = {0};
	SYSTEMTIME system_time = {0};
	GetLocalTime(&system_time);
	_snprintf_s(log_file_path, MAX_PATH - 1, _TRUNCATE, 
		    "..\\logs\\log_%d-%d-%d_%d-%d-%d.txt", 
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
	SHGetPathFromIDListA(pidl, dst->path);

	dst->top = strlen(dst->path);

	ILFree(pidl);
}

int main() {
	Path src = {0};
	Path dst = {0};

	SetSourcePath(&src);
	SetDestinationPath(&dst);

	if(src.top == 0) {
		MessageBox(NULL,
		   	"Invalid source folder.\n"
			"Backup not started.",
		   	NULL,
		   	MB_OK);
		return 1;
	}

	if(dst.top == 0) {
		MessageBox(NULL,
		   	"Invalid destination folder.\n"
			"Backup not started.",
		   	NULL,
		   	MB_OK);
		return 1;
	}

	LogStruct log = {0};
	OpenLogFile(&log);
	if(log.handle == INVALID_HANDLE_VALUE) {
		MessageBox(NULL,
		   	"Log couldn't be created.\n"
			"Backup not started.",
		   	NULL,
		   	MB_OK);
		return 1;
	}

	PushSourceToDestination(&src, &dst);

	// LOG START TIME
	SYSTEMTIME system_time;
	char start_message[64] = {0};
	GetLocalTime(&system_time);
	_snprintf_s(start_message, 64, _TRUNCATE, 
		    "START: Backup started on %d-%d-%d at %d:%d:%d\r\n", 
		    system_time.wMonth,
		    system_time.wDay,
		    system_time.wYear,
		    system_time.wHour,
		    system_time.wMinute,
		    system_time.wSecond);
	LogMessage(&log, start_message);

	BackupDirectoryRecursively(&src, &dst, &log);

	// LOG STATS
	char log_stats_string[256] = {0};
	_snprintf_s(log_stats_string, 256, _TRUNCATE,
		    "STATS:\r\n"
		    "\t%d files checked\r\n"
		    "\t%d folders checked\r\n"
		    "\t%d out of %d files copied.\r\n"
		    "\t%d errors occurred.\r\n",
		    log.files_checked_count,
		    log.folders_checked_count,
		    log.copy_success_count,
		    log.should_copy_count,
		    log.error_count);
	LogMessage(&log, log_stats_string);

	// LOG END TIME
	char end_message[64] = {0};
	GetLocalTime(&system_time);
	_snprintf_s(end_message, 64, _TRUNCATE, 
		    "END:   Backup ended on %d-%d-%d at %d:%d:%d\r\n", 
		    system_time.wMonth,
		    system_time.wDay,
		    system_time.wYear,
		    system_time.wHour,
		    system_time.wMinute,
		    system_time.wSecond);
	LogMessage(&log, end_message);

	ULARGE_INTEGER total_bytes = {0};
	ULARGE_INTEGER free_bytes = {0};
	GetDiskFreeSpaceExA(dst.path, NULL, &total_bytes, &free_bytes);
	total_bytes.QuadPart /= 1024ull * 1024ull * 1024ull;
	free_bytes.QuadPart /= 1024ull * 1024ull * 1024ull;

	char message_stats_string[256] = {0};
	_snprintf_s(message_stats_string, 256, _TRUNCATE,
		    "Backup Complete!!\r\n"
		    "%d files checked\r\n"
		    "%d folders checked\r\n"
		    "%d out of %d files copied.\r\n"
		    "%d errors occurred.\r\n"
		    "%lld free GB\r\n"
		    "%lld total GB\r\n",
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

