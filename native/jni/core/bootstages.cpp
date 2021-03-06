/* bootstages.c - Core bootstage operations
 *
 * All bootstage operations, including simple mount in post-fs,
 * magisk mount in post-fs-data, various image handling, script
 * execution, load modules, install Magisk Manager etc.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <vector>
#include <string>

#include "magisk.h"
#include "db.h"
#include "utils.h"
#include "img.h"
#include "daemon.h"
#include "resetprop.h"
#include "selinux.h"
#include "flags.h"

using namespace std;

static char buf[PATH_MAX], buf2[PATH_MAX];
static vector<string> module_list;
static bool seperate_vendor;

char *system_block, *vendor_block, *magiskloop;

static int bind_mount(const char *from, const char *to);
extern void auto_start_magiskhide();

/***************
 * Magic Mount *
 ***************/

#ifdef MAGISK_DEBUG
#define VLOGI(tag, from, to) LOGI("%s: %s <- %s\n", tag, to, from)
#else
#define VLOGI(tag, from, to) LOGI("%s: %s\n", tag, to)
#endif

// Precedence: MODULE > SKEL > INTER > DUMMY
#define IS_DUMMY   0x01    /* mount from mirror */
#define IS_INTER   0x02    /* intermediate node */
#define IS_SKEL    0x04    /* mount from skeleton */
#define IS_MODULE  0x08    /* mount from module */

#define IS_DIR(n)  ((n)->type == DT_DIR)
#define IS_LNK(n)  ((n)->type == DT_LNK)
#define IS_REG(n)  ((n)->type == DT_REG)

class node_entry {
public:
	explicit node_entry(const char *, uint8_t status = 0, uint8_t type = 0);
	~node_entry();
	void create_module_tree(const char *module);
	void magic_mount();
	node_entry *extract(const char *name);

private:
	const char *module;    /* Only used when status & IS_MODULE */
	const string name;
	uint8_t type;
	uint8_t status;
	node_entry *parent;
	vector<node_entry *> children;

	node_entry(const char *, const char *, uint8_t type);
	bool is_root();
	string get_path();
	node_entry *insert(node_entry *);
	void clone_skeleton();
	int get_path(char *path);
};

node_entry::node_entry(const char *name, uint8_t status, uint8_t type)
		: name(name), type(type), status(status), parent(nullptr) {}

node_entry::node_entry(const char *module, const char *name, uint8_t type)
		: node_entry(name, (uint8_t) 0, type) {
	this->module = module;
}

node_entry::~node_entry() {
	for (auto &node : children)
		delete node;
}

bool node_entry::is_root() {
	return parent == nullptr;
}

string node_entry::get_path() {
	get_path(buf);
	return buf;
}

int node_entry::get_path(char *path) {
	int len = 0;
	if (parent)
		len = parent->get_path(path);
	len += sprintf(path + len, "/%s", name.c_str());
	return len;
}

node_entry *node_entry::insert(node_entry *node) {
	node->parent = this;
	for (auto &child : children) {
		if (child->name == node->name) {
			if (node->status > child->status) {
				// The new node has higher precedence
				delete child;
				child = node;
				return node;
			} else {
				delete node;
				return child;
			}
		}
	}
	children.push_back(node);
	return node;
}

void node_entry::create_module_tree(const char *module) {
	DIR *dir;
	struct dirent *entry;

	auto full_path = get_path();
	snprintf(buf, PATH_MAX, "%s/%s%s", MOUNTPOINT, module, full_path.c_str());

	if (!(dir = xopendir(buf)))
		return;

	while ((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		// Create new node
		auto node = new node_entry(module, entry->d_name, entry->d_type);
		snprintf(buf, PATH_MAX, "%s/%s", full_path.c_str(), entry->d_name);

		/*
		 * Clone the parent in the following condition:
		 * 1. File in module is a symlink
		 * 2. Target file do not exist
		 * 3. Target file is a symlink (exclude /system/vendor)
		 */
		bool clone = false;
		if (IS_LNK(node) || access(buf, F_OK) == -1) {
			clone = true;
		} else if (!is_root() || node->name != "vendor") {
			struct stat s;
			xstat(buf, &s);
			if (S_ISLNK(s.st_mode))
				clone = true;
		}

		if (clone) {
			// Mark self as a skeleton
			status |= IS_SKEL;  /* This will not overwrite if parent is module */
			node->status = IS_MODULE;
		} else if (IS_DIR(node)) {
			// Check if marked as replace
			snprintf(buf2, PATH_MAX, "%s/%s%s/.replace", MOUNTPOINT, module, buf);
			if (access(buf2, F_OK) == 0) {
				// Replace everything, mark as leaf
				node->status = IS_MODULE;
			} else {
				// This will be an intermediate node
				node->status = IS_INTER;
			}
		} else if (IS_REG(node)) {
			// This is a file, mark as leaf
			node->status = IS_MODULE;
		}
		node = insert(node);
		if (node->status & (IS_SKEL | IS_INTER)) {
			// Intermediate folder, travel deeper
			node->create_module_tree(module);
		}
	}
	closedir(dir);
}

void node_entry::clone_skeleton() {
	DIR *dir;
	struct dirent *entry;
	struct node_entry *dummy;

	// Clone the structure
	auto full_path = get_path();
	snprintf(buf, PATH_MAX, "%s%s", MIRRDIR, full_path.c_str());
	if (!(dir = xopendir(buf)))
		return;
	while ((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		// Create dummy node
		dummy = new node_entry(entry->d_name, IS_DUMMY, entry->d_type);
		insert(dummy);
	}
	closedir(dir);

	if (status & IS_SKEL) {
		file_attr attr;
		getattr(full_path.c_str(), &attr);
		LOGI("mnt_tmpfs : %s\n", full_path.c_str());
		xmount("tmpfs", full_path.c_str(), "tmpfs", 0, nullptr);
		setattr(full_path.c_str(), &attr);
	}

	for (auto &child : children) {
		snprintf(buf, PATH_MAX, "%s/%s", full_path.c_str(), child->name.c_str());

		// Create the dummy file/directory
		if (IS_DIR(child))
			xmkdir(buf, 0755);
		else if (IS_REG(child))
			close(creat(buf, 0644));
		// Links will be handled later

		if (is_root() && child->name == "vendor") {
			if (seperate_vendor) {
				cp_afc(MIRRDIR "/system/vendor", "/system/vendor");
				VLOGI("copy_link ", "/system/vendor", MIRRDIR "/system/vendor");
			}
			// Skip
			continue;
		} else if (child->status & IS_MODULE) {
			// Mount from module file to dummy file
			snprintf(buf2, PATH_MAX, "%s/%s%s/%s", MOUNTPOINT,
					 child->module, full_path.c_str(), child->name.c_str());
		} else if (child->status & (IS_SKEL | IS_INTER)) {
			// It's an intermediate folder, recursive clone
			child->clone_skeleton();
			continue;
		} else if (child->status & IS_DUMMY) {
			// Mount from mirror to dummy file
			snprintf(buf2, PATH_MAX, "%s%s/%s", MIRRDIR, full_path.c_str(), child->name.c_str());
		}

		if (IS_LNK(child)) {
			// Copy symlinks directly
			cp_afc(buf2, buf);
			VLOGI("copy_link ", buf2, buf);
		} else {
			snprintf(buf, PATH_MAX, "%s/%s", full_path.c_str(), child->name.c_str());
			bind_mount(buf2, buf);
		}
	}
}

void node_entry::magic_mount() {
	if (status & IS_MODULE) {
		// Mount module item
		auto real_path = get_path();
		snprintf(buf, PATH_MAX, "%s/%s%s", MOUNTPOINT, module, real_path.c_str());
		bind_mount(buf, real_path.c_str());
	} else if (status & IS_SKEL) {
		// The node is labeled to be cloned with skeleton, lets do it
		clone_skeleton();
	} else if (status & IS_INTER) {
		// It's an intermediate node, travel deeper
		for (auto &child : children)
			child->magic_mount();
	}
	// The only thing goes here should be vendor placeholder
	// There should be no dummies, so don't need to handle it here
}

node_entry *node_entry::extract(const char *name) {
	node_entry *node = nullptr;
	// Extract the vendor node out of system tree and swap with placeholder
	for (auto &child : children) {
		if (child->name == name) {
			node = child;
			child = new node_entry(name);
			child->parent = node->parent;
			node->parent = nullptr;
			break;
		}
	}
	return node;
}

/***********
 * setenvs *
 ***********/

static void set_path() {
	char buffer[512];
	sprintf(buffer, BBPATH ":%s", getenv("PATH"));
	setenv("PATH", buffer, 1);
}

static void set_mirror_path() {
	setenv("PATH", BBPATH ":/sbin:" MIRRDIR "/system/bin:"
				   MIRRDIR "/system/xbin:" MIRRDIR "/vendor/bin", 1);
}

/***********
 * Scripts *
 ***********/

static void exec_common_script(const char* stage) {
	DIR *dir;
	struct dirent *entry;
	snprintf(buf2, PATH_MAX, "%s/%s.d", SECURE_DIR, stage);

	if (!(dir = xopendir(buf2)))
		return;
	chdir(buf2);

	while ((entry = xreaddir(dir))) {
		if (entry->d_type == DT_REG) {
			if (access(entry->d_name, X_OK) == -1)
				continue;
			LOGI("%s.d: exec [%s]\n", stage, entry->d_name);
			exec_t exec { .pre_exec = strcmp(stage, "post-fs-data") ? set_path : set_mirror_path };
			int pid = exec_command(exec, MIRRDIR "/system/bin/sh", entry->d_name);
			if (pid != -1)
				waitpid(pid, nullptr, 0);
		}
	}

	closedir(dir);
	chdir("/");
}

static void exec_module_script(const char* stage) {
	for (const auto &m : module_list) {
		const auto module = m.c_str();
		snprintf(buf2, PATH_MAX, "%s/%s/%s.sh", MOUNTPOINT, module, stage);
		if (access(buf2, F_OK) == -1)
			continue;
		LOGI("%s: exec [%s.sh]\n", module, stage);
		exec_t exec { .pre_exec = strcmp(stage, "post-fs-data") ? set_path : set_mirror_path };
		int pid = exec_command(exec, MIRRDIR "/system/bin/sh", buf2);
		if (pid != -1)
			waitpid(pid, nullptr, 0);
	}
}

/****************
 * Simple Mount *
 ****************/

static void simple_mount(const char *path) {
	DIR *dir;
	struct dirent *entry;

	snprintf(buf, PATH_MAX, "%s%s", SIMPLEMOUNT, path);
	if (!(dir = opendir(buf)))
		return;

	while ((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		// Target file path
		snprintf(buf2, PATH_MAX, "%s/%s", path, entry->d_name);
		// Only mount existing file
		if (access(buf2, F_OK) == -1)
			continue;
		if (entry->d_type == DT_DIR) {
			simple_mount(string(buf2).c_str());
		} else if (entry->d_type == DT_REG) {
			// Actual file path
			snprintf(buf, PATH_MAX, "%s%s", SIMPLEMOUNT, buf2);
			// Clone all attributes
			clone_attr(buf2, buf);
			// Finally, mount the file
			bind_mount(buf, buf2);
		}
	}

	closedir(dir);
}

/*****************
 * Miscellaneous *
 *****************/

static int bind_mount(const char *from, const char *to) {
	int ret = xmount(from, to, nullptr, MS_BIND, nullptr);
	VLOGI("bind_mount", from, to);
	return ret;
}

static bool magisk_env() {
	LOGI("* Initializing Magisk environment\n");

	// Alternative binaries paths
	const char *alt_bin[] = { "/cache/data_bin", "/data/magisk",
							  "/data/data/com.topjohnwu.magisk/install",
							  "/data/user_de/0/com.topjohnwu.magisk/install" };
	for (auto &alt : alt_bin) {
		struct stat st;
		if (lstat(alt, &st) != -1) {
			if (S_ISLNK(st.st_mode)) {
				unlink(alt);
				continue;
			}
			rm_rf(DATABIN);;
			cp_afc(alt, DATABIN);
			rm_rf(alt);
			break;
		}
	}

	// Remove legacy stuffs
	unlink("/data/magisk.img");
	unlink("/data/magisk_debug.log");

	// Symlink for legacy path users
	symlink(MAGISKTMP, "/sbin/.core");

	// Create directories in tmpfs overlay
	xmkdirs(MIRRDIR "/system", 0755);
	xmkdir(MIRRDIR "/bin", 0755);
	xmkdir(BBPATH, 0755);
	xmkdir(MOUNTPOINT, 0755);
	xmkdir(BLOCKDIR, 0755);

	// Boot script directories
	xmkdir(SECURE_DIR "/post-fs-data.d", 0755);
	xmkdir(SECURE_DIR "/service.d", 0755);

	LOGI("* Mounting mirrors");
	auto mounts = file_to_vector("/proc/mounts");
	bool system_as_root = false;
	for (auto &line : mounts) {
		if (str_contains(line, " /system_root ")) {
			bind_mount("/system_root/system", MIRRDIR "/system");
			sscanf(line.c_str(), "%s", buf);
			system_block = strdup(buf);
			system_as_root = true;
		} else if (!system_as_root && str_contains(line, " /system ")) {
			sscanf(line.c_str(), "%s %*s %s", buf, buf2);
			system_block = strdup(buf);
			xmount(system_block, MIRRDIR "/system", buf2, MS_RDONLY, nullptr);
			VLOGI("mount", system_block, MIRRDIR "/system");
		} else if (str_contains(line, " /vendor ")) {
			seperate_vendor = true;
			sscanf(line.c_str(), "%s %*s %s", buf, buf2);
			vendor_block = strdup(buf);
			xmkdir(MIRRDIR "/vendor", 0755);
			xmount(vendor_block, MIRRDIR "/vendor", buf2, MS_RDONLY, nullptr);
			VLOGI("mount", vendor_block, MIRRDIR "/vendor");
		} else if (SDK_INT >= 24 &&
		str_contains(line, " /proc ") && !str_contains(line, "hidepid=2")) {
			// Enforce hidepid
			xmount(nullptr, "/proc", nullptr, MS_REMOUNT, "hidepid=2,gid=3009");
		}
	}
	if (!seperate_vendor) {
		xsymlink(MIRRDIR "/system/vendor", MIRRDIR "/vendor");
		VLOGI("link", MIRRDIR "/system/vendor", MIRRDIR "/vendor");
	}

	xmkdirs(DATABIN, 0755);
	bind_mount(DATABIN, MIRRDIR "/bin");
	if (access(MIRRDIR "/bin/busybox", X_OK) == -1)
		return false;
	LOGI("* Setting up internal busybox");
	exec_command_sync(MIRRDIR "/bin/busybox", "--install", "-s", BBPATH);
	xsymlink(MIRRDIR "/bin/busybox", BBPATH "/busybox");

	// Disable/remove magiskhide, resetprop, and modules
	if (SDK_INT < 19) {
		close(xopen(DISABLEFILE, O_RDONLY | O_CREAT, 0));
		unlink("/sbin/resetprop");
		unlink("/sbin/magiskhide");
	}
	return true;
}

static void collect_modules() {
	chdir(MOUNTPOINT);
	DIR *dir = xopendir(".");
	struct dirent *entry;
	while ((entry = xreaddir(dir))) {
		if (entry->d_type == DT_DIR) {
			if (strcmp(entry->d_name, ".") == 0 ||
				strcmp(entry->d_name, "..") == 0 ||
				strcmp(entry->d_name, ".core") == 0 ||
				strcmp(entry->d_name, "lost+found") == 0)
				continue;
			chdir(entry->d_name);
			if (access("remove", F_OK) == 0) {
				chdir("..");
				LOGI("%s: remove\n", entry->d_name);
				rm_rf(entry->d_name);
				continue;
			}
			unlink("update");
			if (access("disable", F_OK))
				module_list.emplace_back(entry->d_name);
			chdir("..");
		}
	}
	closedir(dir);
	chdir("/");
}

static bool prepare_img() {
	const char *alt_img[] =
			{ "/cache/magisk.img", "/data/magisk_merge.img", "/data/adb/magisk_merge.img" };

	for (auto &alt : alt_img) {
		if (merge_img(alt, MAINIMG)) {
			LOGE("Image merge %s -> " MAINIMG " failed!\n", alt);
			return false;
		}
	}

	if (access(MAINIMG, F_OK) == -1) {
		if (create_img(MAINIMG, 64))
			return false;
	}

	LOGI("* Mounting " MAINIMG "\n");
	magiskloop = mount_image(MAINIMG, MOUNTPOINT);
	if (magiskloop == nullptr)
		return false;

	// Migrate legacy boot scripts
	struct stat st;
	if (lstat(LEGACY_CORE "/post-fs-data.d", &st) == 0 && S_ISDIR(st.st_mode)) {
		cp_afc(LEGACY_CORE "/post-fs-data.d", SECURE_DIR "/post-fs-data.d");
		rm_rf(LEGACY_CORE "/post-fs-data.d");
	}
	if (lstat(LEGACY_CORE "/service.d", &st) == 0 && S_ISDIR(st.st_mode)) {
		cp_afc(LEGACY_CORE "/service.d", SECURE_DIR "/service.d");
		rm_rf(LEGACY_CORE "/service.d");
	}

	// Links for legacy paths
	xmkdir(LEGACY_CORE, 0755);
	symlink(SECURE_DIR "/post-fs-data.d", LEGACY_CORE "/post-fs-data.d");
	symlink(SECURE_DIR "/service.d", LEGACY_CORE "/service.d");

	collect_modules();
	return trim_img(MAINIMG, MOUNTPOINT, magiskloop) == 0;
}

static void install_apk(const char *apk) {
	setfilecon(apk, "u:object_r:" SEPOL_FILE_DOMAIN ":s0");
	while (true) {
		sleep(5);
		LOGD("apk_install: attempting to install APK\n");
		exec_t exec { .err = true, .fd = -1 };
		int pid = exec_command(exec, "/system/bin/pm", "install", "-r", apk);
		if (pid != -1) {
			FILE *res = fdopen(exec.fd, "r");
			bool err = false;
			while (fgets(buf, PATH_MAX, res)) {
				LOGD("apk_install: %s", buf);
				err |= strstr(buf, "Error:") != nullptr;
			}
			waitpid(pid, nullptr, 0);
			fclose(res);
			// Keep trying until pm is started
			if (err)
				continue;
			break;
		}
	}
	unlink(apk);
}

static bool check_data() {
	bool mnt = false;
	bool data = false;
	auto mounts = file_to_vector("/proc/mounts");
	for (auto &line : mounts) {
		if (line.find(" /data ") != string::npos &&
			line.find("tmpfs") == string::npos)
			mnt = true;
	}
	if (mnt) {
		auto crypto = getprop("ro.crypto.state");
		if (!crypto.empty()) {
			if (crypto == "unencrypted") {
				// Unencrypted, we can directly access data
				data = true;
			} else {
				// Encrypted, check whether vold is started
				data = !getprop("init.svc.vold").empty();
			}
		} else {
			// ro.crypto.state is not set, assume it's unencrypted
			data = true;
		}
	}
	return data;
}

void unlock_blocks() {
	DIR *dir;
	struct dirent *entry;
	int fd, dev, OFF = 0;

	if ((dev = xopen("/dev/block", O_RDONLY | O_CLOEXEC)) < 0)
		return;
	dir = xfdopendir(dev);

	while((entry = readdir(dir))) {
		if (entry->d_type == DT_BLK) {
			if ((fd = openat(dev, entry->d_name, O_RDONLY)) < 0)
				continue;
			if (ioctl(fd, BLKROSET, &OFF) == -1)
				PLOGE("unlock %s", entry->d_name);
			close(fd);
		}
	}
	close(dev);
}

/****************
 * Entry points *
 ****************/

[[noreturn]] static void unblock_boot_process() {
	close(xopen(UNBLOCKFILE, O_RDONLY | O_CREAT, 0));
	pthread_exit(nullptr);
}

static const char wrapper[] =
"#!/system/bin/sh\n"
"unset LD_LIBRARY_PATH\n"
"unset LD_PRELOAD\n"
"exec /sbin/magisk.bin \"${0##*/}\" \"$@\"\n";

void startup() {
	android_logging();
	if (!check_data())
		unblock_boot_process();

	if (access(SECURE_DIR, F_OK) != 0) {
		/* If the folder is not automatically created by the system,
		 * do NOT proceed further. Manual creation of the folder
		 * will cause bootloops on FBE devices. */
		LOGE(SECURE_DIR " is not present, abort...");
		unblock_boot_process();
	}

#if 0
	// Increment boot count
	int boot_count = 0;
	FILE *cf = fopen(BOOTCOUNT, "r");
	if (cf) {
		fscanf(cf, "%d", &boot_count);
		fclose(cf);
	}
	boot_count++;
	if (boot_count > 2)
		creat(DISABLEFILE, 0644);
	cf = xfopen(BOOTCOUNT, "w");
	fprintf(cf, "%d", boot_count);
	fclose(cf);
#endif

	// No uninstaller or core-only mode
	if (access(DISABLEFILE, F_OK) != 0) {
		simple_mount("/system");
		simple_mount("/vendor");
	}

	// Unlock all blocks for rw
	unlock_blocks();

	LOGI("* Creating /sbin overlay");
	DIR *dir;
	struct dirent *entry;
	int root, sbin, fd;
	void *magisk, *init;
	size_t magisk_size, init_size;

	xmount(nullptr, "/", nullptr, MS_REMOUNT, nullptr);

	// GSIs will have to override /sbin/adbd with /system/bin/adbd
	if (access("/sbin/adbd", F_OK) == 0 && access("/system/bin/adbd", F_OK) == 0) {
		umount2("/sbin/adbd", MNT_DETACH);
		cp_afc("/system/bin/adbd", "/sbin/adbd");
	}

	// Create hardlink mirror of /sbin to /root
	mkdir("/root", 0750);
	clone_attr("/sbin", "/root");
	full_read("/sbin/magisk", &magisk, &magisk_size);
	unlink("/sbin/magisk");
	full_read("/sbin/magiskinit", &init, &init_size);
	unlink("/sbin/magiskinit");
	root = xopen("/root", O_RDONLY | O_CLOEXEC);
	sbin = xopen("/sbin", O_RDONLY | O_CLOEXEC);
	link_dir(sbin, root);
	close(sbin);

	// Mount the /sbin tmpfs overlay
	xmount("tmpfs", "/sbin", "tmpfs", 0, nullptr);
	chmod("/sbin", 0755);
	setfilecon("/sbin", "u:object_r:rootfs:s0");
	sbin = xopen("/sbin", O_RDONLY | O_CLOEXEC);

	// Remove some traces of Magisk
	unlink(MAGISKRC);
	mkdir(MAGISKTMP, 0755);
	cp_afc("/.backup/.magisk", MAGISKTMP "/config");
	rm_rf("/.backup");

	// Create applet symlinks
	for (int i = 0; applet_names[i]; ++i) {
		snprintf(buf, PATH_MAX, "/sbin/%s", applet_names[i]);
		xsymlink("/sbin/magisk", buf);
	}

	// Setup binary and wrapper
	fd = creat("/sbin/magisk.bin", 0755);
	xwrite(fd, magisk, magisk_size);
	close(fd);
	free(magisk);
	unlink("/sbin/magisk");
	fd = creat("/sbin/magisk", 0755);
	xwrite(fd, wrapper, sizeof(wrapper) - 1);
	close(fd);
	setfilecon("/sbin/magisk.bin", "u:object_r:" SEPOL_FILE_DOMAIN ":s0");
	setfilecon("/sbin/magisk", "u:object_r:" SEPOL_FILE_DOMAIN ":s0");

	// Setup magiskinit symlinks
	fd = creat("/sbin/magiskinit", 0755);
	xwrite(fd, init, init_size);
	close(fd);
	free(init);
	setfilecon("/sbin/magiskinit", "u:object_r:" SEPOL_FILE_DOMAIN ":s0");
	for (int i = 0; init_applet[i]; ++i) {
		snprintf(buf, PATH_MAX, "/sbin/%s", init_applet[i]);
		xsymlink("/sbin/magiskinit", buf);
	}

	// Create symlinks pointing back to /root
	dir = xfdopendir(root);
	while((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		snprintf(buf, PATH_MAX, "/root/%s", entry->d_name);
		xsymlinkat(buf, sbin, entry->d_name);
	}

	close(sbin);
	close(root);

	// Start post-fs-data mode
	execl("/sbin/magisk.bin", "magisk", "--post-fs-data", nullptr);
}

[[noreturn]] static inline void core_only() {
	auto_start_magiskhide();
	unblock_boot_process();
}

void post_fs_data(int client) {
	// ack
	write_int(client, 0);
	close(client);

	xmount(nullptr, "/", nullptr, MS_REMOUNT | MS_RDONLY, nullptr);

	LOGI("** post-fs-data mode running\n");

	if (!magisk_env()) {
		LOGE("* Magisk environment setup incomplete, abort\n");
		unblock_boot_process();
	}

	start_log_daemon();

	// Run common scripts
	LOGI("* Running post-fs-data.d scripts\n");
	exec_common_script("post-fs-data");

	// Core only mode
	if (access(DISABLEFILE, F_OK) == 0)
		core_only();

	if (!prepare_img()) {
		LOGE("* Magisk image mount failed, switch to core-only mode\n");
		free(magiskloop);
		magiskloop = nullptr;
		creat(DISABLEFILE, 0644);
	}

	restorecon();
	chmod(SECURE_DIR, 0700);

	// Execute module scripts
	LOGI("* Running module post-fs-data scripts\n");
	exec_module_script("post-fs-data");

	// Recollect modules
	module_list.clear();
	collect_modules();

	// Create the system root entry
	auto sys_root = new node_entry("system", IS_INTER);

	// Vendor root entry
	node_entry *ven_root = nullptr;

	bool has_modules = false;

	LOGI("* Loading modules\n");
	for (const auto &m : module_list) {
		const auto module = m.c_str();
		// Read props
		snprintf(buf, PATH_MAX, "%s/%s/system.prop", MOUNTPOINT, module);
		if (access(buf, F_OK) == 0) {
			LOGI("%s: loading [system.prop]\n", module);
			load_prop_file(buf, false);
		}
		// Check whether enable auto_mount
		snprintf(buf, PATH_MAX, "%s/%s/auto_mount", MOUNTPOINT, module);
		if (access(buf, F_OK) == -1)
			continue;
		// Double check whether the system folder exists
		snprintf(buf, PATH_MAX, "%s/%s/system", MOUNTPOINT, module);
		if (access(buf, F_OK) == -1)
			continue;

		// Construct structure
		has_modules = true;
		LOGI("%s: constructing magic mount structure\n", module);
		// If /system/vendor exists in module, create a link outside
		snprintf(buf, PATH_MAX, "%s/%s/system/vendor", MOUNTPOINT, module);
		if (access(buf, F_OK) == 0) {
			snprintf(buf2, PATH_MAX, "%s/%s/vendor", MOUNTPOINT, module);
			unlink(buf2);
			xsymlink(buf, buf2);
		}
		sys_root->create_module_tree(module);
	}

	if (has_modules) {
		// Pull out /system/vendor node if exist
		ven_root = sys_root->extract("vendor");

		// Magic!!
		sys_root->magic_mount();
		if (ven_root) ven_root->magic_mount();
	}

	// Cleanup memory
	delete sys_root;
	delete ven_root;

	core_only();
}

void late_start(int client) {
	LOGI("** late_start service mode running\n");
	// ack
	write_int(client, 0);
	close(client);

	if (access(SECURE_DIR, F_OK) != 0) {
		// It's safe to create the folder at this point if the system didn't create it
		xmkdir(SECURE_DIR, 0700);
		// And reboot to make proper setup possible
		exec_command_sync("/system/bin/reboot");
	}

	auto_start_magiskhide();

	// Run scripts after full patch, most reliable way to run scripts
	LOGI("* Running service.d scripts\n");
	exec_common_script("service");

	// Core only mode
	if (access(DISABLEFILE, F_OK) == 0)
		goto core_only;

	LOGI("* Running module service scripts\n");
	exec_module_script("service");

core_only:
	if (access(MANAGERAPK, F_OK) == 0) {
		// Install Magisk Manager if exists
		rename(MANAGERAPK, "/data/magisk.apk");
		install_apk("/data/magisk.apk");
	} else {
		// Check whether we have a valid manager installed
		db_strings str;
		get_db_strings(&str, SU_MANAGER);
		if (validate_manager(str[SU_MANAGER], 0, nullptr)) {
			// There is no manager installed, install the stub
			exec_command_sync("/sbin/magiskinit", "-x", "manager", "/data/magisk.apk");
			install_apk("/data/magisk.apk");
		}
	}

	// All boot stage done, cleanup
	module_list.clear();
}

void boot_complete(int client) {
	LOGI("** boot_complete triggered\n");
	// ack
	write_int(client, 0);
	close(client);

	unlink(BOOTCOUNT);
}
