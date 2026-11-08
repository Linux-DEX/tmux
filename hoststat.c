/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Linux-DEX
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/statvfs.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tmux.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <mach/mach.h>
#endif

static int
host_stat_percent(uint64_t used, uint64_t total)
{
	if (total == 0 || used == 0)
		return (0);
	if (used > total)
		used = total;
	return ((int)((used * 100) / total));
}

static void
host_stat_rate(char *buf, size_t len, uint64_t rate)
{
	if (rate >= 1000000000)
		xsnprintf(buf, len, "%lluG", (unsigned long long)(rate / 1000000000));
	else if (rate >= 1000000)
		xsnprintf(buf, len, "%lluM", (unsigned long long)(rate / 1000000));
	else if (rate >= 1000)
		xsnprintf(buf, len, "%lluK", (unsigned long long)(rate / 1000));
	else
		xsnprintf(buf, len, "%llu", (unsigned long long)rate);
}

/* 8/16G when the unit matches, otherwise 800M/16G. */
static void
host_stat_pair(char *buf, size_t len, uint64_t used, uint64_t total)
{
	char		us[8], ts[8];
	uint64_t	un, tn;
	char		uu, tu;

	un = used;
	uu = 'B';
	if (used >= 1024ULL * 1024 * 1024 * 1024) {
		un = used / (1024ULL * 1024 * 1024 * 1024);
		uu = 'T';
	} else if (used >= 1024ULL * 1024 * 1024) {
		un = used / (1024ULL * 1024 * 1024);
		uu = 'G';
	} else if (used >= 1024ULL * 1024) {
		un = used / (1024ULL * 1024);
		uu = 'M';
	} else if (used >= 1024ULL) {
		un = used / 1024ULL;
		uu = 'K';
	}
	tn = total;
	tu = 'B';
	if (total >= 1024ULL * 1024 * 1024 * 1024) {
		tn = total / (1024ULL * 1024 * 1024 * 1024);
		tu = 'T';
	} else if (total >= 1024ULL * 1024 * 1024) {
		tn = total / (1024ULL * 1024 * 1024);
		tu = 'G';
	} else if (total >= 1024ULL * 1024) {
		tn = total / (1024ULL * 1024);
		tu = 'M';
	} else if (total >= 1024ULL) {
		tn = total / 1024ULL;
		tu = 'K';
	}
	xsnprintf(us, sizeof us, "%llu", (unsigned long long)un);
	xsnprintf(ts, sizeof ts, "%llu%c", (unsigned long long)tn, tu);
	if (uu == tu)
		xsnprintf(buf, len, "%s/%s", us, ts);
	else
		xsnprintf(buf, len, "%s%c/%s", us, uu, ts);
}

static void
host_stat_rates(struct host_stat *st, uint64_t rx, uint64_t tx)
{
	static uint64_t	prx, ptx;
	static time_t	when;
	static int	ok;
	time_t		now = time(NULL);
	uint64_t	down = 0, up = 0, dt;

	if (ok && now > when) {
		dt = (uint64_t)(now - when);
		if (rx >= prx)
			down = (rx - prx) / dt;
		if (tx >= ptx)
			up = (tx - ptx) / dt;
	}
	prx = rx;
	ptx = tx;
	when = now;
	ok = 1;
	host_stat_rate(st->net_down, sizeof st->net_down, down);
	host_stat_rate(st->net_up, sizeof st->net_up, up);
	host_stat_rate(st->net, sizeof st->net, down + up);
}

#if defined(__APPLE__)

static void
host_stat_net_bytes(uint64_t *rx, uint64_t *tx)
{
	int			 mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
	size_t			 len = 0;
	char			*buf, *next, *lim;
	struct if_msghdr	*ifm;
	struct if_msghdr2	*if2;
	struct sockaddr_dl	*sdl;

	*rx = *tx = 0;
	if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0)
		return;
	buf = xmalloc(len);
	if (sysctl(mib, 6, buf, &len, NULL, 0) != 0) {
		free(buf);
		return;
	}
	lim = buf + len;
	for (next = buf; next + 4 <= lim; ) {
		ifm = (struct if_msghdr *)next;
		if (ifm->ifm_msglen < 4 || next + ifm->ifm_msglen > lim)
			break;
		next += ifm->ifm_msglen;
		if (ifm->ifm_type != RTM_IFINFO2 ||
		    ifm->ifm_msglen < sizeof *if2)
			continue;
		if2 = (struct if_msghdr2 *)ifm;
		sdl = (struct sockaddr_dl *)(if2 + 1);
		if ((char *)(sdl + 1) > next)
			continue;
		if (sdl->sdl_nlen >= 2 && memcmp(sdl->sdl_data, "lo", 2) == 0)
			continue;
		*rx += if2->ifm_data.ifi_ibytes;
		*tx += if2->ifm_data.ifi_obytes;
	}
	free(buf);
}

static void
host_stat_read(struct host_stat *st)
{
	host_cpu_load_info_data_t	 load;
	mach_msg_type_number_t		 count;
	vm_statistics64_data_t		 vm;
	vm_size_t			 pagesize;
	struct statfs			 fs;
	uint64_t			 mem = 0, used, rx, tx, bsize, blocks, avail;
	size_t				 slen = sizeof mem;
	static uint64_t			 ptot, pbusy;
	static int			 phave;
	uint64_t			 user, sys, idle, nice, tot, busy;

	memset(st, 0, sizeof *st);
	xsnprintf(st->net, sizeof st->net, "0");
	xsnprintf(st->net_down, sizeof st->net_down, "0");
	xsnprintf(st->net_up, sizeof st->net_up, "0");
	xsnprintf(st->ram_usage, sizeof st->ram_usage, "0/0B");
	xsnprintf(st->disk_usage, sizeof st->disk_usage, "0/0B");
	count = HOST_CPU_LOAD_INFO_COUNT;
	if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
	    (host_info_t)&load, &count) == KERN_SUCCESS) {
		user = load.cpu_ticks[CPU_STATE_USER];
		sys = load.cpu_ticks[CPU_STATE_SYSTEM];
		idle = load.cpu_ticks[CPU_STATE_IDLE];
		nice = load.cpu_ticks[CPU_STATE_NICE];
		tot = user + sys + idle + nice;
		busy = user + sys + nice;
		if (phave && tot > ptot)
			st->cpu = host_stat_percent(busy - pbusy, tot - ptot);
		ptot = tot;
		pbusy = busy;
		phave = 1;
	}

	count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
	    (host_info64_t)&vm, &count) == KERN_SUCCESS &&
	    host_page_size(mach_host_self(), &pagesize) == KERN_SUCCESS &&
	    sysctlbyname("hw.memsize", &mem, &slen, NULL, 0) == 0) {
		used = (uint64_t)(vm.active_count + vm.wire_count +
		    vm.compressor_page_count) * pagesize;
		st->ram = host_stat_percent(used, mem);
		host_stat_pair(st->ram_usage, sizeof st->ram_usage, used, mem);
	}

	if (statfs("/", &fs) == 0 && fs.f_blocks > 0) {
		bsize = fs.f_bsize != 0 ? fs.f_bsize : 512;
		blocks = (uint64_t)fs.f_blocks * bsize;
		avail = (uint64_t)fs.f_bavail * bsize;
		used = blocks > avail ? blocks - avail : 0;
		st->disk = host_stat_percent(used, blocks);
		host_stat_pair(st->disk_usage, sizeof st->disk_usage, used, blocks);
	}

	host_stat_net_bytes(&rx, &tx);
	host_stat_rates(st, rx, tx);
}

#elif defined(__linux__)

static void
host_stat_net_bytes(uint64_t *rx, uint64_t *tx)
{
	FILE		*f;
	char		 line[256], name[64];
	unsigned long long in, out, skip;

	*rx = *tx = 0;
	if ((f = fopen("/proc/net/dev", "r")) == NULL)
		return;
	if (fgets(line, sizeof line, f) == NULL ||
	    fgets(line, sizeof line, f) == NULL) {
		fclose(f);
		return;
	}
	while (fgets(line, sizeof line, f) != NULL) {
		if (sscanf(line, " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		    name, &in, &skip, &skip, &skip, &skip, &skip, &skip, &skip,
		    &out) != 10)
			continue;
		if (strcmp(name, "lo") == 0)
			continue;
		*rx += in;
		*tx += out;
	}
	fclose(f);
}

static void
host_stat_read(struct host_stat *hs)
{
	FILE			*f;
	char			 line[256];
	unsigned long long	 u, n, s, i, w, irq, soft, steal, kb;
	unsigned long long	 tot, prev, idle, total = 0, avail = 0;
	static unsigned long long pu, pn, ps, pi, pw, pirq, psoft, pst;
	static int		 phave;
	struct statvfs		 vfs;
	uint64_t		 used, rx, tx, blocks, freeb;

	memset(hs, 0, sizeof *hs);
	xsnprintf(hs->net, sizeof hs->net, "0");
	xsnprintf(hs->net_down, sizeof hs->net_down, "0");
	xsnprintf(hs->net_up, sizeof hs->net_up, "0");
	xsnprintf(hs->ram_usage, sizeof hs->ram_usage, "0/0B");
	xsnprintf(hs->disk_usage, sizeof hs->disk_usage, "0/0B");
	if ((f = fopen("/proc/stat", "r")) != NULL) {
		if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		    &u, &n, &s, &i, &w, &irq, &soft, &steal) == 8) {
			tot = u + n + s + i + w + irq + soft + steal;
			prev = pu + pn + ps + pi + pw + pirq + psoft + pst;
			if (phave && tot > prev) {
				idle = (i + w) - (pi + pw);
				hs->cpu = host_stat_percent((tot - prev) - idle,
				    tot - prev);
			}
			pu = u; pn = n; ps = s; pi = i; pw = w;
			pirq = irq; psoft = soft; pst = steal;
			phave = 1;
		}
		fclose(f);
	}

	if ((f = fopen("/proc/meminfo", "r")) != NULL) {
		while (fgets(line, sizeof line, f) != NULL) {
			if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
				total = kb * 1024;
			else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
				avail = kb * 1024;
		}
		fclose(f);
		if (total > avail) {
			used = total - avail;
			hs->ram = host_stat_percent(used, total);
			host_stat_pair(hs->ram_usage, sizeof hs->ram_usage,
			    used, total);
		}
	}

	if (statvfs("/", &vfs) == 0 && vfs.f_blocks > vfs.f_bavail) {
		blocks = (uint64_t)vfs.f_blocks * vfs.f_frsize;
		freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
		used = blocks > freeb ? blocks - freeb : 0;
		hs->disk = host_stat_percent(used, blocks);
		host_stat_pair(hs->disk_usage, sizeof hs->disk_usage, used,
		    blocks);
	}
	host_stat_net_bytes(&rx, &tx);
	host_stat_rates(hs, rx, tx);
}

#else

static void
host_stat_read(struct host_stat *st)
{
	memset(st, 0, sizeof *st);
	xsnprintf(st->net, sizeof st->net, "0");
	xsnprintf(st->net_down, sizeof st->net_down, "0");
	xsnprintf(st->net_up, sizeof st->net_up, "0");
	xsnprintf(st->ram_usage, sizeof st->ram_usage, "0/0B");
	xsnprintf(st->disk_usage, sizeof st->disk_usage, "0/0B");
}

#endif

/* One sample a second. CPU and network need two samples, so the first is 0. */
void
host_stat_sample(struct host_stat *st)
{
	static time_t		last;
	static struct host_stat cached;
	time_t			now = time(NULL);

	if (last == 0 || now != last) {
		host_stat_read(&cached);
		last = now;
	}
	memcpy(st, &cached, sizeof *st);
}
