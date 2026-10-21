/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private TUI save helpers. No shell is used for filenames or clipboard text. */
#ifndef SGIL1_TUI_SAVE_H
#define SGIL1_TUI_SAVE_H
#include <sys/random.h>
#include <sys/wait.h>
#include <sys/xattr.h>

enum tui_save_phase { TUI_SAVE_NONE, TUI_SAVE_PATH, TUI_SAVE_CHOICE,
	TUI_SAVE_WRITING, TUI_SAVE_DONE, TUI_SAVE_ERROR };

struct tui_save {
	bool initialized, capture_pending, snapshot_valid, request_pending, copy_requested, append, existed, leds;
	int action_mode;
	enum tui_save_phase phase;
	char input[PATH_MAX], path[PATH_MAX], directory[PATH_MAX];
	char basename[NAME_MAX + 1], suggested[80], message[384];
	char clipboard[PATH_MAX], copy_error[256];
	int clipboard_kind, directory_fd, copy_error_fd;
	pid_t copy_pid;
	uint64_t copy_deadline;
	size_t copy_error_len, cursor, path_scroll, bytes, entries;
	char *data;
	struct stat before, parent;
};

static void tui_save_reset(struct tui_save *save)
{
	if (!save->initialized)
		return;
	if (save->directory_fd >= 0)
		close(save->directory_fd);
	if (save->copy_pid > 0) {
		kill(save->copy_pid, SIGKILL);
		while (waitpid(save->copy_pid, NULL, 0) < 0 && errno == EINTR) {}
		close(save->copy_error_fd);
	}
	free(save->data);
	memset(save, 0, sizeof(*save));
}

static void tui_save_error(struct tui_save *save, const char *operation, int error)
{
	snprintf(save->message, sizeof(save->message), "%s%s%s", operation,
		 error ? ": " : "", error ? strerror(error) : "");
	save->phase = TUI_SAVE_ERROR;
	save->path_scroll = 0;
	if (save->directory_fd >= 0) {
		close(save->directory_fd);
		save->directory_fd = -1;
	}
}

static bool tui_save_directory_probe(const char *path)
{
	uint64_t random;
	char name[80];
	int directory = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
	int fd;
	bool valid;

	if (directory < 0)
		return false;
	if (getrandom(&random, sizeof(random), 0) != sizeof(random)) {
		close(directory);
		return false;
	}
	snprintf(name, sizeof(name), ".sgil1ctl-check-%ld-%016" PRIx64, (long)getpid(), random);
	fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	valid = fd >= 0;
	if (fd >= 0) {
		close(fd);
		valid = !unlinkat(directory, name, 0);
	}
	close(directory);
	return valid;
}

static bool tui_save_default_directory(char *out, size_t size)
{
	const char *candidates[] = { getenv("TMPDIR"), getenv("TMP"),
		getenv("XDG_RUNTIME_DIR"), "/var/tmp", "/tmp" };
	size_t i;
	struct stat st;
	char resolved[PATH_MAX];

	out[0] = '\0';
	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (!candidates[i] || !*candidates[i] ||
		    !realpath(candidates[i], resolved) || stat(resolved, &st) ||
		    !S_ISDIR(st.st_mode) ||
		    faccessat(AT_FDCWD, resolved, W_OK | X_OK, AT_EACCESS) ||
		    !tui_save_directory_probe(resolved))
			continue;
		if (snprintf(out, size, "%s", resolved) < (int)size)
			return true;
	}
	return false;
}

static bool tui_save_find_program(const char *name, char *out, size_t size)
{
	const char *path = getenv("PATH");
	char *copy = strdup(path ? path : "/usr/bin:/bin"), *part, *cursor;
	struct stat st;
	bool found = false;

	if (!copy)
		return false;
	cursor = copy;
	while ((part = strsep(&cursor, ":"))) {
		if (snprintf(out, size, "%s/%s", *part ? part : ".", name) >= (int)size)
			continue;
		if (!stat(out, &st) && S_ISREG(st.st_mode) &&
		    !faccessat(AT_FDCWD, out, X_OK, AT_EACCESS)) {
			found = true;
			break;
		}
	}
	free(copy);
	if (!found)
		out[0] = '\0';
	return found;
}

static void tui_save_find_clipboard(struct tui_save *save)
{
	save->clipboard_kind = 0;
	if (tui_save_find_program("pbcopy", save->clipboard, sizeof(save->clipboard)))
		save->clipboard_kind = 1;
	else if (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY") &&
		 tui_save_find_program("wl-copy", save->clipboard, sizeof(save->clipboard)))
		save->clipboard_kind = 2;
	else if (getenv("DISPLAY") && *getenv("DISPLAY") &&
		 tui_save_find_program("xclip", save->clipboard, sizeof(save->clipboard)))
		save->clipboard_kind = 3;
	else if (getenv("DISPLAY") && *getenv("DISPLAY") &&
		 tui_save_find_program("xsel", save->clipboard, sizeof(save->clipboard)))
		save->clipboard_kind = 4;
}

static bool tui_save_same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
		a->st_size == b->st_size && a->st_mode == b->st_mode &&
		a->st_uid == b->st_uid && a->st_gid == b->st_gid && a->st_nlink == b->st_nlink &&
		a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
		a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* Resolve the parent once and anchor subsequent operations to its directory fd. */
static int tui_save_prepare(struct tui_save *save)
{
	char candidate[PATH_MAX], parent[PATH_MAX], resolved[PATH_MAX];
	const char *input = *save->input ? save->input : save->suggested;
	char *slash;
	int length;

	if (save->directory_fd >= 0) {
		close(save->directory_fd);
		save->directory_fd = -1;
	}
	save->path[0] = '\0';
	if (*input == '/')
		length = snprintf(candidate, sizeof(candidate), "%s", input);
	else if (input[0] == '~' && input[1] == '/' && getenv("HOME") && *getenv("HOME") == '/')
		length = snprintf(candidate, sizeof(candidate), "%s/%s", getenv("HOME"), input + 2);
	else {
		if (!tui_save_default_directory(save->directory, sizeof(save->directory))) {
			tui_save_error(save, "No writable default directory; enter an absolute file path", 0);
			return -1;
		}
		length = snprintf(candidate, sizeof(candidate), "%s/%s", save->directory, input);
	}
	if (length < 0 || length >= (int)sizeof(candidate)) {
		tui_save_error(save, "File path is too long", ENAMETOOLONG);
		return -1;
	}
	snprintf(save->path, sizeof(save->path), "%s", candidate);
	snprintf(parent, sizeof(parent), "%s", candidate);
	slash = strrchr(parent, '/');
	if (!slash || !slash[1] || !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..") ||
	    strlen(slash + 1) > NAME_MAX) {
		tui_save_error(save, "Enter a file path, not a directory", 0);
		return -1;
	}
	snprintf(save->basename, sizeof(save->basename), "%s", slash + 1);
	if (slash == parent)
		slash[1] = '\0';
	else
		*slash = '\0';
	if (!realpath(parent, resolved)) {
		tui_save_error(save, "Cannot access destination directory", errno);
		return -1;
	}
	length = snprintf(save->path, sizeof(save->path), "%s%s%s", resolved,
		 !strcmp(resolved, "/") ? "" : "/", save->basename);
	if (length < 0 || length >= (int)sizeof(save->path)) {
		tui_save_error(save, "Resolved file path is too long", ENAMETOOLONG);
		return -1;
	}
	snprintf(save->directory, sizeof(save->directory), "%s", resolved);
	save->directory_fd = open(resolved, O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (save->directory_fd < 0 || fstat(save->directory_fd, &save->parent) ||
	    faccessat(save->directory_fd, ".", W_OK | X_OK, AT_EACCESS)) {
		tui_save_error(save, "Cannot write to destination directory", errno);
		return -1;
	}
	save->existed = false;
	if (!fstatat(save->directory_fd, save->basename, &save->before, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(save->before.st_mode) || save->before.st_nlink != 1 ||
		    (save->before.st_mode & (S_ISUID | S_ISGID))) {
			tui_save_error(save, "Choose a regular file without symlinks, hard links or privilege bits", 0);
			return -1;
		}
		if (faccessat(save->directory_fd, save->basename, W_OK, AT_EACCESS)) {
			tui_save_error(save, "Destination file is not writable", errno);
			return -1;
		}
		save->existed = true;
	} else if (errno != ENOENT) {
		tui_save_error(save, "Cannot inspect destination file", errno);
		return -1;
	}
	save->path_scroll = 0;
	return save->existed && save->before.st_size > 0 ? 1 : 0;
}

static int tui_save_write_all(int fd, const char *data, size_t bytes)
{
	while (bytes) {
		ssize_t written = write(fd, data, bytes > SSIZE_MAX ? SSIZE_MAX : bytes);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			if (!written)
				errno = EIO;
			return -1;
		}
		data += written;
		bytes -= (size_t)written;
	}
	return 0;
}

static int tui_save_copy_attributes(int source, int target, const struct stat *original)
{
	struct stat st;
	char *names = NULL, *value = NULL, *name;
	ssize_t size, got;
	int ret = -1, saved_errno;

	if (fstat(target, &st) ||
	    ((st.st_uid != original->st_uid || st.st_gid != original->st_gid) &&
	     fchown(target, original->st_uid, original->st_gid)))
		return -1;
	/* Do not retain a staging directory's inherited ACL instead of the file's ACL. */
	if (fremovexattr(target, "system.posix_acl_access") && errno != ENODATA && errno != ENOTSUP)
		return -1;
	size = flistxattr(source, NULL, 0);
	if (size < 0 && errno != ENOTSUP)
		return -1;
	if (size > 0) {
		names = malloc((size_t)size);
		if (!names || (got = flistxattr(source, names, (size_t)size)) < 0)
			goto out;
		for (name = names; name < names + got; name += strlen(name) + 1) {
			if (!strcmp(name, "security.capability")) {
				errno = EPERM;
				goto out;
			}
			size = fgetxattr(source, name, NULL, 0);
			if (size < 0)
				goto out;
			value = malloc(size ? (size_t)size : 1);
			if (!value || (size = fgetxattr(source, name, value, (size_t)size)) < 0)
				goto out;
			{
				char *current = malloc(size ? (size_t)size : 1);
				bool same = current && fgetxattr(target, name, current, (size_t)size) == size &&
					!memcmp(current, value, (size_t)size);

				free(current);
				if (!same && fsetxattr(target, name, value, (size_t)size, 0))
					goto out;
			}
			free(value);
			value = NULL;
		}
	}
	ret = fchmod(target, original->st_mode & 0777);
out:
	saved_errno = errno;
	free(names);
	free(value);
	errno = saved_errno;
	return ret;
}

/* Stage both overwrite and append; failed writes never truncate the old file. */
static int tui_save_commit(struct tui_save *save)
{
	char stage[80] = "", buffer[65536];
	struct stat st;
	int output = -1, source = -1, error = 0;
	unsigned int attempt;
	const char *operation = "Cannot save buffer";
	bool staged = false;

	if (stat(save->directory, &st) || st.st_dev != save->parent.st_dev || st.st_ino != save->parent.st_ino) {
		tui_save_error(save, "Destination directory changed; choose the path again", 0);
		return -1;
	}
	if (save->existed) {
		source = openat(save->directory_fd, save->basename, (save->append ? O_RDONLY : O_WRONLY) | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (source < 0 || fstat(source, &st) || !tui_save_same_file(&st, &save->before)) {
			if (source >= 0)
				errno = EAGAIN;
			operation = "Destination changed or cannot be opened safely; choose the path again";
			goto fail;
		}
		if (flock(source, LOCK_EX | LOCK_NB)) {
			operation = "Destination is busy; try again later";
			goto fail;
		}
	}
	for (attempt = 0; attempt < 32; attempt++) {
		uint64_t random;

		if (getrandom(&random, sizeof(random), 0) != sizeof(random)) {
			operation = "Cannot obtain a safe temporary filename";
			goto fail;
		}
		snprintf(stage, sizeof(stage), ".sgil1ctl-save-%ld-%016" PRIx64, (long)getpid(), random);
		output = openat(save->directory_fd, stage, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (output >= 0) {
			staged = true;
			break;
		}
		if (errno != EEXIST)
			break;
	}
	if (output < 0) {
		operation = "Cannot create a private staging file in the destination directory";
		goto fail;
	}
	if (save->append && source >= 0) {
		ssize_t count;

		for (;;) {
			count = read(source, buffer, sizeof(buffer));
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0) {
				operation = "Cannot read the existing file for safe append";
				goto fail;
			}
			if (!count)
				break;
			if (tui_save_write_all(output, buffer, (size_t)count))
				goto fail;
		}
	}
	if (tui_save_write_all(output, save->data ? save->data : "", save->bytes))
		goto fail;
	if (source >= 0 && tui_save_copy_attributes(source, output, &save->before)) {
		operation = "Cannot safely preserve destination ownership, permissions or attributes";
		goto fail;
	}
	if (fsync(output))
		goto fail;
	if (close(output)) {
		output = -1;
		goto fail;
	}
	output = -1;
	if (save->existed) {
		if (fstatat(save->directory_fd, save->basename, &st, AT_SYMLINK_NOFOLLOW) ||
		    !tui_save_same_file(&st, &save->before)) {
			errno = EAGAIN;
			operation = "Destination changed while saving; original data was not replaced";
			goto fail;
		}
		if (renameat(save->directory_fd, stage, save->directory_fd, save->basename))
			goto fail;
	} else if (renameat2(save->directory_fd, stage, save->directory_fd, save->basename, RENAME_NOREPLACE)) {
		operation = "Cannot create destination without replacing another file";
		goto fail;
	}
	staged = false;
	if (source >= 0)
		close(source);
	snprintf(save->message, sizeof(save->message), "%s %zu bytes. Up/Down scroll path.",
		 save->append ? "Appended" : "Saved", save->bytes);
	{
		int directory = openat(save->directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (directory < 0 || fsync(directory))
			snprintf(save->message, sizeof(save->message), "Saved %zu bytes; directory sync unavailable: %s",
				 save->bytes, strerror(errno));
		if (directory >= 0)
			close(directory);
	}
	close(save->directory_fd);
	save->directory_fd = -1;
	save->phase = TUI_SAVE_DONE;
	tui_save_find_clipboard(save);
	return 0;
fail:
	error = errno;
	if (output >= 0)
		close(output);
	if (source >= 0)
		close(source);
	if (staged)
		unlinkat(save->directory_fd, stage, 0);
	tui_save_error(save, operation, error);
	return -1;
}
static void tui_save_copy_start(struct tui_save *save, uint64_t now)
{
	int input[2] = { -1, -1 }, errors[2] = { -1, -1 };
	pid_t child;

	save->copy_requested = false;
	if (save->copy_pid > 0)
		return;
	tui_save_find_clipboard(save);
	if (!save->clipboard_kind) {
		snprintf(save->message, sizeof(save->message), "No clipboard helper is available in this session");
		return;
	}
	if (pipe2(input, O_CLOEXEC) || pipe2(errors, O_CLOEXEC | O_NONBLOCK))
		goto fail;
	/* A path fits in a Linux pipe; preload before fork so SIGPIPE cannot escape. */
	if (tui_save_write_all(input[1], save->path, strlen(save->path)))
		goto fail;
	child = fork();
	if (child < 0)
		goto fail;
	if (!child) {
		char *args[] = { save->clipboard, NULL, NULL, NULL, NULL };

		dup2(input[0], STDIN_FILENO);
		dup2(errors[1], STDOUT_FILENO);
		dup2(errors[1], STDERR_FILENO);
		close(input[0]); close(input[1]); close(errors[0]); close(errors[1]);
		if (sgil1_lock_fd >= 0)
			close(sgil1_lock_fd);
		if (save->clipboard_kind == 3) {
			args[1] = "-selection"; args[2] = "clipboard"; args[3] = "-in";
		} else if (save->clipboard_kind == 4) {
			args[1] = "--clipboard"; args[2] = "--input";
		}
		execv(save->clipboard, args);
		dprintf(STDERR_FILENO, "Cannot start clipboard helper: %s", strerror(errno));
		_exit(127);
	}
	close(input[0]); close(input[1]); close(errors[1]);
	save->copy_pid = child;
	save->copy_error_fd = errors[0];
	save->copy_error_len = 0;
	save->copy_error[0] = '\0';
	save->copy_deadline = now + 5000;
	snprintf(save->message, sizeof(save->message), "Copying file path...");
	return;
fail:
	{
		int error = errno;
		if (input[0] >= 0) close(input[0]);
		if (input[1] >= 0) close(input[1]);
		if (errors[0] >= 0) close(errors[0]);
		if (errors[1] >= 0) close(errors[1]);
		snprintf(save->message, sizeof(save->message), "Cannot copy path: %s", strerror(error));
	}
}

static void tui_save_copy_drain(struct tui_save *save)
{
	ssize_t count;

	while (save->copy_error_len + 1 < sizeof(save->copy_error) &&
	       (count = read(save->copy_error_fd, save->copy_error + save->copy_error_len,
		 sizeof(save->copy_error) - save->copy_error_len - 1)) > 0)
		save->copy_error_len += (size_t)count;
	save->copy_error[save->copy_error_len] = '\0';
}

static void tui_save_copy_poll(struct tui_save *save, uint64_t now)
{
	int status;
	pid_t result;

	if (save->copy_pid <= 0)
		return;
	tui_save_copy_drain(save);
	result = waitpid(save->copy_pid, &status, WNOHANG);
	if (!result) {
		if (save->copy_deadline && now >= save->copy_deadline) {
			kill(save->copy_pid, SIGKILL);
			save->copy_deadline = 0;
			snprintf(save->message, sizeof(save->message), "Clipboard helper timed out; saved file is unchanged");
		}
		return;
	}
	if (result < 0 && errno == EINTR)
		return;
	tui_save_copy_drain(save); /* Include diagnostics written just before exit. */
	if (save->copy_deadline) {
		if (result > 0 && WIFEXITED(status) && !WEXITSTATUS(status))
			snprintf(save->message, sizeof(save->message), "File path copied to clipboard. Return closes this view.");
		else {
			size_t i;
			for (i = 0; i < save->copy_error_len; i++)
				if ((unsigned char)save->copy_error[i] < 32 || save->copy_error[i] == 127)
					save->copy_error[i] = ' ';
			snprintf(save->message, sizeof(save->message), "Clipboard failed: %s",
				 save->copy_error_len ? save->copy_error : "helper exited unsuccessfully; check desktop access");
		}
	}
	close(save->copy_error_fd);
	save->copy_pid = 0;
}
#endif
