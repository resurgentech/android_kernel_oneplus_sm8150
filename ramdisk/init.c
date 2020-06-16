#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifdef _FILE_OFFSET_BITS
#undef _FILE_OFFSET_BITS
#endif
#define _FILE_OFFSET_BITS 64

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

struct part_info {
	int major;
	int minor;
	char partname[64];
	char devname[64];
};

static void parse_uevent(const char *path, struct part_info *pinfo)
{
	char buf[4096];
	char *str, *value, *tmp, *target, *equal;
	int fd;
	size_t len, ret;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open file %s: %s\n", path, strerror(errno));
		return;
	}

	len = 0;
	while ((ret = read(fd, buf + len, sizeof(buf)))) {
		len += ret;
		if (len >= sizeof(buf)) {
			printf("%s: buffer overflow\n", path);
			return;
		}
	}
	buf[len - 1] = '\0';
	memset(pinfo, 0, sizeof(struct part_info));

	target = buf;
	while ((str = strtok_r(target, "\n", &equal))) {
		target = NULL;
		tmp = strchr(str, '=');
		if (tmp == NULL || tmp + 1 == NULL)
			continue;

		value = tmp + 1;
		*tmp = '\0';

		// str is now the key
		if (strcmp(str, "MAJOR") == 0) {
			pinfo->major = atoi(value);
		} else if (strcmp(str, "MINOR") == 0) {
			pinfo->minor = atoi(value);
		} else if (strcmp(str, "DEVNAME") == 0) {
			strcpy(pinfo->devname, value);
		} else if (strcmp(str, "PARTNAME") == 0) {
			strcpy(pinfo->partname, value);
		}
	}

	close(fd);
}

static int swap_major;
static int swap_minor;
static int ram0_major;
static int ram0_minor;

static void search_partitions(char *name)
{
	static int depth;
	struct dirent *p;
	struct part_info pinfo;
	DIR *d;
	char path[PATH_MAX];

	if (name == NULL)
		name = "/sys/dev/block";

	d = opendir(name);
	if (!d)
		return;

	while ((p = readdir(d))) {
		/* Skip the names "." and ".." as we don't want to recurse on them. */
		if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
			continue;
		}
		// get real path
		sprintf(path, "%s/%s", name, p->d_name);

		if (depth == 1 && !strcmp(p->d_name, "uevent")) {
			parse_uevent(path, &pinfo);

			if (!strcmp(pinfo.partname, "swap")) {
				swap_major = pinfo.major;
				swap_minor = pinfo.minor;
			} else if (!strcmp(pinfo.devname, "ram0")) {
				ram0_major = pinfo.major;
				ram0_minor = pinfo.minor;
			}
		} else if (p->d_type == DT_LNK || p->d_type == DT_DIR) {
			depth++;
			if (depth != 2)
				search_partitions(path);
			depth--;
		}
	}

	closedir(d);
}

int main(int argc, char *argv[])
{
	int ret;

	// Initialize KMSG
	mknod("/kmsg", S_IFCHR | 0755, makedev(1, 11));
	int kmsg_fd = open("/kmsg", O_WRONLY);
	if (kmsg_fd == -1) {
		printf("Failed to open /dev/kmsg, ignoring...\n");
	} else {
		fcntl(kmsg_fd, F_SETFD, FD_CLOEXEC);
		close(1);
		dup2(kmsg_fd, 1);
		setbuf(stdout, NULL);
		close(2);
		dup2(kmsg_fd, 2);
		setbuf(stderr, NULL);
	}

	ret = mkdir("/sys", 0755);
	if (ret < 0) {
		perror("Failed to mkdir /sys");
		return 1;
	}

	ret = mount("sysfs", "/sys", "sysfs", 0, NULL);
	if (ret < 0) {
		perror("Failed to mount /sys");
		return 1;
	}

	while (true) {
		search_partitions(NULL);
		if (swap_major && ram0_major)
			break;

		printf("Retrying partition probe\n");
		sleep(1);
	}

	ret = mknod("/swap", 0644 | S_IFBLK, makedev(swap_major, swap_minor));
	if (ret < 0) {
		perror("Failed to mknod /swap");
		return 1;
	}

	ret = mknod("/ram0", 0644 | S_IFBLK, makedev(ram0_major, ram0_minor));
	if (ret < 0) {
		perror("Failed to mknod /ram0");
		return 1;
	}

	// Copy swap to ram0
	#define BUFFER_SIZE (4 * 1024L * 1024L) // 4 MiB
	char *buf = malloc(BUFFER_SIZE);
	if (!buf) {
		perror("Failed to malloc buffer");
		return 1;
	}

	// Copy dpolicy to original image
	int from = open("/swap", O_RDONLY);
	if (from < 0) {
		perror("Failed to open swap");
		return 1;
	}
	int to = open("/ram0", O_RDWR);
	if (to < 0) {
		perror("Failed to open ram0");
		return 1;
	}
	ssize_t len;
	while ((len = read(from, buf, BUFFER_SIZE)) > 0)
		write(to, buf, len);
	close(from);
	close(to);
	free(buf);
	sync();

	umount("/sys");

	// Mount ram0
	mkdir("/root", 0755);
	ret = mount("/ram0", "/root", "ext4", 0, NULL);
	if (ret < 0) {
		perror("Failed to mount system");
		return 1;
	}

	ret = chdir("/root");
	if (ret < 0) {
		perror("Failed to cd /root");
		return 1;
	}

	ret = mount("/root", "/", NULL, MS_MOVE, NULL);
	if (ret < 0) {
		perror("Failed to move mount /mnt to /");
		return 1;
	}

	ret = chroot(".");
	if (ret < 0) {
		perror("Failed to chroot");
		return 1;
	}

	execv(argv[0], argv);
	perror("Failed to exec");

	return 1;
}
