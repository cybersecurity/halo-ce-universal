/* the AArch64 kernel's struct stat (fstat, newfstatat), spelled with
fixed-width types so the ILP32 guest reads the kernel's layout */
struct kstat {
	unsigned long long st_dev;
	unsigned long long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned long long st_rdev;
	unsigned long long __pad;
	long long st_size;
	int st_blksize;
	int __pad2;
	long long st_blocks;
	long long st_atime_sec;
	long long st_atime_nsec;
	long long st_mtime_sec;
	long long st_mtime_nsec;
	long long st_ctime_sec;
	long long st_ctime_nsec;
	unsigned int __unused[2];
};
