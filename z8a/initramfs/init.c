/*
 * Static aarch64 init for the XTC Z8A watch: mounts the eMMC userdata
 * partition, attaches /data/debian.img to /dev/loop0, bind-mounts the
 * virtual filesystems and switches the root over to the Debian rootfs.
 *
 * Freestanding: raw syscalls only, linked with -nostdlib -static.
 */

#define AT_FDCWD        (-100)
#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2

#define SYS_dup3        24
#define SYS_ioctl       29
#define SYS_mknodat     33
#define SYS_mkdirat     34
#define SYS_mount       40
#define SYS_chdir       49
#define SYS_chroot      51
#define SYS_openat      56
#define SYS_close       57
#define SYS_write       64
#define SYS_nanosleep   101
#define SYS_exit        93
#define SYS_execve      221

#define MS_BIND         4096
#define MS_MOVE         8192
#define MS_REC          16384

#define LOOP_SET_FD     0x4C00
#define S_IFBLK         0060000

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

static long sys3(long n, long a, long b, long c)
{
	return sys6(n, a, b, c, 0, 0, 0);
}

static long sys2(long n, long a, long b)
{
	return sys6(n, a, b, 0, 0, 0, 0);
}

static long sys1(long n, long a)
{
	return sys6(n, a, 0, 0, 0, 0, 0);
}

static int slen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static int con_fd = -1;
static int con_fd2 = -1;

static void msg(const char *s)
{
	if (con_fd >= 0)
		sys3(SYS_write, con_fd, (long)s, slen(s));
	if (con_fd2 >= 0)
		sys3(SYS_write, con_fd2, (long)s, slen(s));
}

static void msgnum(long v)
{
	char buf[24];
	int k = 0, a, b;

	if (v == 0) {
		buf[k++] = '0';
	} else {
		int neg = 0;
		if (v < 0) {
			neg = 1;
			v = -v;
		}
		while (v > 0 && k < 20) {
			buf[k++] = (char)('0' + (v % 10));
			v /= 10;
		}
		if (neg)
			buf[k++] = '-';
	}
	for (a = 0, b = k - 1; a < b; a++, b--) {
		char t = buf[a];
		buf[a] = buf[b];
		buf[b] = t;
	}
	buf[k++] = '\n';
	buf[k] = 0;
	msg(buf);
}

static long open_r(const char *p)
{
	return sys6(SYS_openat, AT_FDCWD, (long)p, O_RDONLY, 0, 0, 0);
}

static long open_rw(const char *p)
{
	return sys6(SYS_openat, AT_FDCWD, (long)p, O_RDWR, 0, 0, 0);
}

static long open_w(const char *p)
{
	return sys6(SYS_openat, AT_FDCWD, (long)p, O_WRONLY, 0, 0, 0);
}

static void sleep_ms(long ms)
{
	long ts[2];
	ts[0] = ms / 1000;
	ts[1] = (ms % 1000) * 1000000L;
	sys2(SYS_nanosleep, (long)ts, 0);
}

static long do_mount(const char *src, const char *tgt, const char *fstype,
		     unsigned long flags, const char *data)
{
	return sys6(SYS_mount, (long)src, (long)tgt, (long)fstype, flags,
		    (long)data, 0);
}

static void mkdir_p(const char *p)
{
	sys3(SYS_mkdirat, AT_FDCWD, (long)p, 0755);
}

static int file_exists(const char *p)
{
	long fd = open_r(p);
	if (fd < 0)
		return 0;
	sys1(SYS_close, fd);
	return 1;
}

void _start(void)
{
	char *argv_init[] = { "/sbin/init", 0 };
	char *envp[] = { "HOME=/", "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
			 "TERM=linux", 0 };
	long img, loop;
	long r;
	int i;

	do_mount("proc", "/proc", "proc", 0, 0);
	do_mount("sysfs", "/sys", "sysfs", 0, 0);
	do_mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
	do_mount("tmpfs", "/tmp", "tmpfs", 0, 0);
	do_mount("configfs", "/sys/kernel/config", "configfs", 0, 0);

	con_fd = (int)open_w("/dev/console");
	con_fd2 = (int)open_w("/dev/ttyGS0");
	msg("\n=== z8a init: static C initramfs ===\n");

	for (i = 0; i < 50; i++) {
		if (file_exists("/dev/mmcblk0p57"))
			break;
		sleep_ms(200);
	}
	if (!file_exists("/dev/mmcblk0p57")) {
		msg("=== eMMC partition missing ===\n");
		goto shell;
	}
	msg("=== eMMC partition present ===\n");

	mkdir_p("/mnt/u");
	mkdir_p("/mnt/root");
	if (do_mount("/dev/mmcblk0p57", "/mnt/u", "ext4", 0, 0) < 0) {
		msg("=== mount userdata FAILED ===\n");
		goto shell;
	}
	msg("=== userdata mounted ===\n");

	sys6(SYS_mknodat, AT_FDCWD, (long)"/dev/loop0", S_IFBLK | 0600, 0x700, 0, 0);
	img = open_rw("/mnt/u/debian.img");
	if (img < 0) {
		img = open_r("/mnt/u/debian.img");
		msg("=== debian.img opened read-only ===\n");
	}
	if (img < 0) {
		msg("=== debian.img not found ===\n");
		goto shell;
	}
	loop = open_rw("/dev/loop0");
	if (loop < 0) {
		msg("=== /dev/loop0 missing ===\n");
		goto shell;
	}
	r = sys3(SYS_ioctl, loop, LOOP_SET_FD, img);
	if (r < 0) {
		msg("=== LOOP_SET_FD failed: ");
		msgnum(r);
		goto shell;
	}
	sys1(SYS_close, loop);
	sys1(SYS_close, img);
	msg("=== loop0 attached ===\n");

	r = do_mount("/dev/loop0", "/mnt/root", "ext4", 0, 0);
	if (r < 0) {
		msg("=== mount debian.img FAILED: ");
		msgnum(r);
		msg("=== retrying read-only ===\n");
		r = do_mount("/dev/loop0", "/mnt/root", "ext4", 1 /*MS_RDONLY*/, 0);
		if (r < 0) {
			msg("=== ro mount also failed: ");
			msgnum(r);
			goto shell;
		}
	}
	msg("=== debian.img mounted ===\n");

	mkdir_p("/mnt/root/dev");
	mkdir_p("/mnt/root/proc");
	mkdir_p("/mnt/root/sys");
	mkdir_p("/mnt/root/run");
	mkdir_p("/mnt/root/tmp");
	mkdir_p("/mnt/root/data");

	do_mount("/dev", "/mnt/root/dev", 0, MS_BIND | MS_REC, 0);
	do_mount("/proc", "/mnt/root/proc", 0, MS_BIND | MS_REC, 0);
	do_mount("/sys", "/mnt/root/sys", 0, MS_BIND | MS_REC, 0);
	do_mount("tmpfs", "/mnt/root/run", "tmpfs", 0, "mode=755");
	do_mount("tmpfs", "/mnt/root/tmp", "tmpfs", 0, "mode=1777");
	do_mount("/mnt/u", "/mnt/root/data", 0, MS_BIND | MS_REC, 0);
	msg("=== binds done, switching root ===\n");

	/*
	 * Move the process into the new root *before* MS_MOVE: chroot(".") uses
	 * the cwd inode, and the old root dentry would otherwise stay pinned.
	 */
	if (sys1(SYS_chdir, (long)"/mnt/root") < 0) {
		msg("=== chdir /mnt/root failed ===\n");
		goto shell;
	}
	r = do_mount("/mnt/root", "/", 0, MS_MOVE, 0);
	if (r < 0) {
		msg("=== MS_MOVE failed: ");
		msgnum(r);
		goto shell;
	}
	sys1(SYS_chroot, (long)".");
	sys1(SYS_chdir, (long)"/");
	msg("=== exec /sbin/init ===\n");
	r = sys3(SYS_execve, (long)"/sbin/init", (long)argv_init, (long)envp);
	msg("=== exec /sbin/init errno=");
	msgnum(r);
	r = sys3(SYS_execve, (long)"/lib/systemd/systemd", (long)argv_init, (long)envp);
	msg("=== exec /lib/systemd/systemd errno=");
	msgnum(r);
	r = sys3(SYS_execve, (long)"/bin/sh", (long)argv_init, (long)envp);
	msg("=== exec /bin/sh errno=");
	msgnum(r);

shell:
	{
		char *argv[] = { "/bin/sh", "-i", 0 };
		long tty = open_rw("/dev/ttyGS0");
		if (tty < 0)
			tty = open_rw("/dev/console");
		if (tty >= 0) {
			sys3(SYS_dup3, tty, 0, 0);
			sys3(SYS_dup3, tty, 1, 0);
			sys3(SYS_dup3, tty, 2, 0);
			if (tty > 2)
				sys1(SYS_close, tty);
		}
		sys3(SYS_execve, (long)"/bin/sh", (long)argv, (long)envp);
	}
	for (;;)
		sleep_ms(1000);
}
