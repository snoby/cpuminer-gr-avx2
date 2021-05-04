#include "virtual_memory.h"
#include "miner.h" // applog
#include "stdio.h"

#ifdef __MINGW32__
// Windows
#include <ntsecapi.h>
#include <tchar.h>
#include <windows.h>
#include <winsock2.h>
/*****************************************************************
SetLockPagesPrivilege: a function to obtain or
release the privilege of locking physical pages.
Inputs:
HANDLE hProcess: Handle for the process for which the
privilege is needed
BOOL bEnable: Enable (TRUE) or disable?
Return value: TRUE indicates success, FALSE failure.
*****************************************************************/
/**
 * AWE Example:
 * https://msdn.microsoft.com/en-us/library/windows/desktop/aa366531(v=vs.85).aspx
 * Creating a File Mapping Using Large Pages:
 * https://msdn.microsoft.com/en-us/library/aa366543(VS.85).aspx
 */
static BOOL SetLockPagesPrivilege() {
  HANDLE token;

  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    applog(LOG_NOTICE, "Huge Pages: Failed to open process token.");
    return FALSE;
  }

  TOKEN_PRIVILEGES tp;
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME,
                            &(tp.Privileges[0].Luid))) {
    applog(LOG_NOTICE, "Huge Pages: Failed to lookup privilege table.");
    return FALSE;
  }

  BOOL rc = AdjustTokenPrivileges(token, FALSE, (PTOKEN_PRIVILEGES)&tp, 0, NULL,
                                  NULL);
  if (!rc || GetLastError() != ERROR_SUCCESS) {
    applog(LOG_NOTICE, "Huge Pages: Failed to adjust privelege token.");
    return FALSE;
  }

  CloseHandle(token);

  return TRUE;
}

static LSA_UNICODE_STRING StringToLsaUnicodeString(LPCTSTR string) {
  LSA_UNICODE_STRING lsaString;

  const DWORD dwLen = (DWORD)wcslen(string);
  lsaString.Buffer = (LPWSTR)string;
  lsaString.Length = (USHORT)((dwLen) * sizeof(WCHAR));
  lsaString.MaximumLength = (USHORT)((dwLen + 1) * sizeof(WCHAR));
  return lsaString;
}

static BOOL ObtainLockPagesPrivilege() {
  HANDLE token;
  PTOKEN_USER user = NULL;

  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD size = 0;

    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (size) {
      user = (PTOKEN_USER)LocalAlloc(LPTR, size);
    }

    GetTokenInformation(token, TokenUser, user, size, &size);
    CloseHandle(token);
  }

  if (!user) {
    applog(LOG_NOTICE, "Huge Pages: Failed token query.");
    return FALSE;
  }

  LSA_HANDLE handle;
  LSA_OBJECT_ATTRIBUTES attributes;
  ZeroMemory(&attributes, sizeof(attributes));

  BOOL result = FALSE;
  if (LsaOpenPolicy(NULL, &attributes, POLICY_ALL_ACCESS, &handle) == 0) {
    LSA_UNICODE_STRING str = StringToLsaUnicodeString(_T(SE_LOCK_MEMORY_NAME));
    applog(LOG_NOTICE, "LSA: '%s'", str.Buffer);
    NTSTATUS status = LsaAddAccountRights(handle, user->User.Sid, &str, 1);
    if (status == 0) {
      applog(LOG_NOTICE,
             "Huge pages support was successfully enabled, but reboot "
             "is required to use it");
      result = TRUE;
    } else {
      applog(LOG_NOTICE, "Huge pages: Failed to add account rights %lu",
             LsaNtStatusToWinError(status));
    }

    LsaClose(handle);
  }

  LocalFree(user);
  return result;
}

static BOOL TrySetLockPagesPrivilege() {
  if (SetLockPagesPrivilege()) {
    return TRUE;
  }
  return ObtainLockPagesPrivilege() && SetLockPagesPrivilege();
}

bool InitHugePages(size_t threads) { return TrySetLockPagesPrivilege(); }

void *AllocateLargePagesMemory(size_t size) {
  const size_t min = GetLargePageMinimum();
  void *mem = NULL;
  if (min > 0) {
    mem = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                       PAGE_READWRITE);
  } else {
    mem = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  }
  return mem;
}

#else
// Linux
#include <sys/mman.h>
static inline int read_hp(const char *path) {
  FILE *fd;
  fd = fopen(path, "r");
  if (fd == NULL) {
    return -1;
  }

  uint64_t value = 0;
  fscanf(fd, "%lu", &value);
  fclose(fd);
  if (ferror(fd) != 0) {
    return -2;
  }
  return (int)value;
}

static inline bool write_hp(const char *path, uint64_t value) {
  FILE *fd;
  fd = fopen(path, "w");
  if (fd == NULL) {
    return false;
  }

  fprintf(fd, "%lu", value);
  fclose(fd);
  if (ferror(fd) != 0) {
    return false;
  }
  return true;
}

// One thread should allocate 2 MiB of Large Pages.
bool InitHugePages(size_t threads) {
  const char *free_path = "/sys/devices/system/node/node0/hugepages/"
                          "hugepages-2048kB/free_hugepages";
  int available_pages = read_hp(free_path);
  if (available_pages < 0) {
    return false;
  }
  if (available_pages >= (int)threads) {
    return true;
  }
  const char *nr_path = "/sys/devices/system/node/node0/hugepages/"
                        "hugepages-2048kB/nr_hugepages";
  int set_pages = read_hp(nr_path);
  set_pages = set_pages < 0 ? 0 : set_pages;
  return write_hp(nr_path, (size_t)set_pages + threads - available_pages);
}

#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
void *AllocateLargePagesMemory(size_t size) {
#if defined(__FreeBSD__)
  void *mem =
      mmap(0, size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED_SUPER | MAP_PREFAULT_READ,
           -1, 0);
#else
  void *mem = mmap(0, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE |
                       MAP_HUGE_2MB,
                   0, 0);
#endif

  if (mem == MAP_FAILED) {
    if (huge_pages) {
      applog(LOG_ERR,
             "Huge Pages allocation failed. Run with root privileges.");
    }

    // Retry without huge pages.
#if defined(__FreeBSD__)
    mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
               0);
#else
    mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
               0);
#endif
  }

  return mem == MAP_FAILED ? NULL : mem;
}

#endif // __MINGW32__

void *AllocateMemory(size_t size) {
  void *mem = AllocateLargePagesMemory(size);
  if (mem == NULL) {
    if (opt_debug) {
      applog(LOG_NOTICE, "Using malloc as allocation method");
    }
    mem = malloc(size);
  }
  if (mem == NULL) {
    applog(LOG_ERR, "Could not allocate any memory for thread");
    exit(1);
  }
  return mem;
}
