// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "erofs/print.h"
#include "liberofs_fanotify.h"

int erofs_fanotify_init_precontent(void)
{
	int fan_fd;

	fan_fd = fanotify_init(FAN_CLASS_PRE_CONTENT | FAN_CLOEXEC | FAN_NONBLOCK,
			       O_RDONLY | O_LARGEFILE);
	if (fan_fd < 0) {
		erofs_err("fanotify_init failed: %s", strerror(errno));
		return -errno;
	}

	return fan_fd;
}

int erofs_fanotify_mark_file(int fan_fd, const char *path)
{
	int err;

	err = fanotify_mark(fan_fd, FAN_MARK_ADD, FAN_PRE_ACCESS, AT_FDCWD, path);
	if (err < 0) {
		erofs_err("fanotify_mark failed for %s: %s", path, strerror(errno));
		return -errno;
	}

	erofs_dbg("Marked %s for FAN_PRE_ACCESS monitoring", path);
	return 0;
}

int erofs_fanotify_parse_range_event(const struct fanotify_event_metadata *meta,
				     struct erofs_fanotify_range *range)
{
	const struct fanotify_event_info_header *info_hdr;
	const struct fanotify_event_info_range *range_info;
	const char *ptr, *end;

	if (meta->metadata_len > meta->event_len) {
		erofs_err("Invalid fanotify metadata length");
		return -EIO;
	}

	if (meta->vers != FANOTIFY_METADATA_VERSION) {
		erofs_err("Unsupported fanotify metadata version %d", meta->vers);
		return -EINVAL;
	}

	/* Initialize range to full file (will be overridden if range info present) */
	range->offset = 0;
	range->count = 0;

	/* Parse additional info records for range information */
	ptr = (const char *)meta + meta->metadata_len;
	end = (const char *)meta + meta->event_len;

	while (ptr < end) {
		size_t info_len;

		if (end - ptr < sizeof(*info_hdr)) {
			erofs_err("Incomplete fanotify event info header");
			return -EIO;
		}
		info_hdr = (const struct fanotify_event_info_header *)ptr;
		info_len = info_hdr->len;
		if (info_len < sizeof(*info_hdr) || ptr + info_len > end) {
			erofs_err("Invalid fanotify event info length");
			return -EIO;
		}

		if (info_hdr->info_type == FAN_EVENT_INFO_TYPE_RANGE) {
			if (info_len < sizeof(*range_info)) {
				erofs_err("Incomplete fanotify range info");
				return -EIO;
			}
			range_info = (const struct fanotify_event_info_range *)ptr;
			range->offset = range_info->offset;
			range->count = range_info->count;
			break;
		}

		ptr += info_hdr->len;
	}

	return 0;
}

int erofs_fanotify_respond(int fan_fd, int event_fd, bool allow)
{
	struct fanotify_response response = {
		.fd = event_fd,
		.response = allow ? FAN_ALLOW : FAN_DENY,
	};
	ssize_t ret;

	ret = write(fan_fd, &response, sizeof(response));
	if (ret != sizeof(response)) {
		erofs_err("Failed to respond to fanotify event: %s",
			  ret < 0 ? strerror(errno) : "short write");
		return ret < 0 ? -errno : -EIO;
	}

	return 0;
}
