/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
#ifndef __EROFS_LIB_LIBEROFS_FANOTIFY_H
#define __EROFS_LIB_LIBEROFS_FANOTIFY_H

#include "erofs/defs.h"
#include <sys/fanotify.h>

/* FAN_PRE_ACCESS may not be defined in older headers */
#ifndef FAN_PRE_ACCESS
#define FAN_PRE_ACCESS 0x00100000
#endif

#ifndef FAN_CLASS_PRE_CONTENT
#define FAN_CLASS_PRE_CONTENT 0x00000008
#endif

#ifndef FAN_EVENT_INFO_TYPE_RANGE
#define FAN_EVENT_INFO_TYPE_RANGE 6
#endif

/* Define struct fanotify_event_info_range if not in system headers */
#ifndef HAVE_STRUCT_FANOTIFY_EVENT_INFO_RANGE
struct fanotify_event_info_range {
	struct fanotify_event_info_header hdr;
	__u32 pad;
	__u64 offset;
	__u64 count;
};
#endif

struct erofs_fanotify_range {
	u64 offset;
	u64 count;
};

/* Initialize fanotify with FAN_CLASS_PRE_CONTENT */
int erofs_fanotify_init_precontent(void);

/* Mark file for FAN_PRE_ACCESS monitoring */
int erofs_fanotify_mark_file(int fan_fd, const char *path);

/* Parse a single fanotify event and extract range information */
int erofs_fanotify_parse_range_event(const struct fanotify_event_metadata *meta,
				     struct erofs_fanotify_range *range);

/* Respond to fanotify permission event */
int erofs_fanotify_respond(int fan_fd, int event_fd, bool allow);

#endif
