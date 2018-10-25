/*
 * (c) 2018 Martin Wilck, SUSE Linux GmbH
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE 1
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include "spookyhash_api.h"

/*
 *  */

#define SECONDS 10
#define BUSY 1000000L
#define IDLE 10000L

struct lock_ops {
	void *(*new)(void);
	void (*free)(void*);
	int (*lock)(void*, const char*, bool);
};

struct lock_ctx {
	void *ctx;
	const struct lock_ops *ops;
};

#define FCNTL_OPEN_DIR "/run/lockscale"

struct fcntl_open_lock {
	int dirfd;
	int fd;
};

static void *new_fcntl_open(void)
{
	struct fcntl_open_lock *p;
	int fd;

	if (mkdir(FCNTL_OPEN_DIR, 0755 != 0) && errno != EEXIST)
		return NULL;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	fd = open(FCNTL_OPEN_DIR, O_RDONLY|O_DIRECTORY);
	if (fd == -1) {
		free(p);
		return NULL;
	};

	p->dirfd = fd;
	return p;
}

static void free_fcntl_open(void *p)
{
	struct fcntl_open_lock *l = p;

	close(l->dirfd);
	free(l);
}

static int lock_fcntl_open(void *ctx, const char *name, bool lock)
{
	int fd;
	struct fcntl_open_lock *octx = ctx;
	struct flock fl = {
		.l_type = (lock ? F_WRLCK : F_UNLCK),
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
		.l_pid = 0,
	};

	if (lock) {
		fd = openat(octx->dirfd, name,
			    O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NOCTTY, 0600);
		if (fd == -1)
			return -errno;
	} else
		fd =octx->fd;

	if (fcntl(fd, F_OFD_SETLKW, &fl) == -1)
		return -errno;

	if (lock)
		octx->fd = fd;
	else {
		close(fd);
		octx->fd = -1;
	}
	return 0;
}

static const struct lock_ops lock_fcntl_ops = {
	.new = new_fcntl_open,
	.free = free_fcntl_open,
	.lock = lock_fcntl_open,
};

struct flock_lock {
	int dirfd;
	int fd;
};

static void *new_flock(void)
{
	struct flock_lock *p;
	int fd;

	if (mkdir(FCNTL_OPEN_DIR, 0755 != 0) && errno != EEXIST)
		return NULL;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	fd = open(FCNTL_OPEN_DIR, O_RDONLY|O_DIRECTORY);
	if (fd == -1) {
		free(p);
		return NULL;
	};

	p->dirfd = fd;
	return p;
}

static void free_flock(void *p)
{
	struct flock_lock *l = p;

	close(l->dirfd);
	free(l);
}

static int lock_flock(void *ctx, const char *name, bool lock)
{
	int fd;
	struct flock_lock *octx = ctx;

	if (lock) {
		fd = openat(octx->dirfd, name,
			    O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NOCTTY, 0600);
		if (fd == -1)
			return -errno;
	} else
		fd =octx->fd;

	if (flock(fd, lock ? LOCK_EX : LOCK_UN) == -1)
		return -errno;

	if (lock)
		octx->fd = fd;
	else {
		close(fd);
		octx->fd = -1;
	}
	return 0;
}

static const struct lock_ops lock_flock_ops = {
	.new = new_flock,
	.free = free_flock,
	.lock = lock_flock,
};

struct sysv_lock {
	int semid;
	uint16_t mask;
};

static int get_semmsl_bit(void)
{
	static const char proc_sem[] = "/proc/sys/kernel/sem";
	long msl, mns, opm, mni;
	FILE *f = fopen(proc_sem, "r");
	int bit = 15;
	long mask = 1 << bit;

	if (fscanf(f, "%ld %ld %ld %ld", &msl, &mns, &opm, &mni) != 4)
		return -1;
	for (bit = 15, mask = 1 << bit; bit; bit--, mask >>= 1) {
		if (msl & mask)
			return bit;
	}
	return 0;
}

static void *new_sysv(void) {
	int id, i;
	key_t key = ftok(FCNTL_OPEN_DIR, 2);
	struct sysv_lock *sl;
	unsigned short *val;
	int bit, nsem;

	if (key == (key_t)-1)
		return NULL;
	sl = calloc(1, sizeof(*sl));
	if (sl == NULL)
		return NULL;
	id = semget(key, 1, 0);
	if (id != -1) {
		semctl(id, 0, IPC_RMID, 0);
	}
	bit = get_semmsl_bit();
	if (bit <= 0)
		return NULL;
	nsem = (1 << bit);
	id = semget(key, nsem, IPC_CREAT|0600);
	if (id == -1)
		goto out_sl;
	val = malloc(nsem * sizeof(*val));
	if (val == NULL)
		goto out_sem;
	for (i = 0; i < nsem; i++)
		val[i] = 1;
	if (semctl(id, 0, SETALL, val))
		goto out_val;
	free(val);
	sl->mask = nsem - 1;
	sl->semid = id;
	return sl;
out_val:
	free(val);
out_sem:
	semctl(id, 0, IPC_RMID, 0);
out_sl:
	free(sl);
	return NULL;
}

static void free_sysv(void *arg)
{
	struct sysv_lock *sl = arg;

	free(sl);
}

static uint16_t _hash(const char *name, uint16_t mask)
{
	uint64_t h = spookyhash_64(name, strlen(name), 0xdeadbeeffeedbaadULL);

	return h & mask;
}

static int lock_sysv(void *c, const char *name, bool lock)
{
	struct sysv_lock *sl = c;
	struct sembuf sb;

	sb.sem_num = _hash(name, sl->mask);
	sb.sem_op = lock ? -1 : 1;
	sb.sem_flg = 0;

	if (semop(sl->semid, &sb, 1) == -1) {
		if (errno != EINTR)
			perror("semop");
		return -errno;
	}
	return 0;
}

static const struct lock_ops lock_sysv_ops = {
	.new = new_sysv,
	.free = free_sysv,
	.lock = lock_sysv,
};

const long BILLION = 1000000000;
static int timespec_subtract (struct timespec *result,
			      struct timespec *x, struct timespec *y)
{

	/* Perform the carry for the later subtraction by updating Y. */
	if (x->tv_nsec < y->tv_nsec) {
		int nsec = (y->tv_nsec - x->tv_nsec) / BILLION + 1;

		y->tv_nsec -= BILLION * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_nsec - y->tv_nsec > BILLION) {
		int nsec = (x->tv_nsec - y->tv_nsec) / BILLION;

		y->tv_nsec += BILLION * nsec;
		y->tv_sec -= nsec;
       }

	/* Compute the time remaining to wait.
	   ‘tv_nsec’ is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_nsec = x->tv_nsec - y->tv_nsec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

static bool stop = false;
void sig_handler(int _unused)
{
//	fprintf(stderr, "ouch %d\n", getpid());
	stop = true;
}

static int busywait(long ns)
{
	struct timespec until, now, diff;
	int done;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += ns / BILLION;
	until.tv_nsec += ns % BILLION;
	if (until.tv_sec >= BILLION) {
		until.tv_sec += until.tv_nsec / BILLION;
		until.tv_nsec = until.tv_nsec % BILLION;
	}
	do {
		clock_gettime(CLOCK_REALTIME, &now);
		done = timespec_subtract(&diff, &until, &now);
	} while (!done && !stop);

	if (done)
		return 0;
	else {
		errno = EINTR;
		return -1;
	}
}

static struct lock_ctx *new_lock_ctx(const struct lock_ops *ops)
{
	struct lock_ctx *lc = calloc(1, sizeof(*lc));

	if (lc == NULL)
		return NULL;
	lc->ops = ops;
	lc->ctx = ops->new();
	if (lc->ctx == NULL) {
		free(lc);
		return NULL;
	}
	return lc;
}

static void free_lock_ctx(struct lock_ctx *lc)
{
	lc->ops->free(lc->ctx);
	free(lc);
}

struct options {
	long busy;
	long idle;
	int seconds;
	int workers;
	int time;
	long total;
	long mini;
	long maxi;
	double avg;
	double stddev;
};

static void worker(const struct lock_ctx *ctx, const struct options *opts,
		   long *result)
{
	static const char NAME[] = "lockscale test";

	int i, rc;
	const struct timespec wt = {
		.tv_sec = opts->idle / BILLION,
		.tv_nsec = opts->idle % BILLION,
	};
	const struct timespec bwt = {
		.tv_sec = opts->busy / BILLION,
		.tv_nsec = opts->busy % BILLION,
	};
	pid_t me = getpid();

	for (i = 0; !stop;  i++) {
		rc = ctx->ops->lock(ctx->ctx, NAME, true);
		if (rc != 0) {
			if (rc == -EINTR)
				continue;
			fprintf(stderr, "%s(%d): %s in lock\n", __func__, me,
				strerror(-rc));
			return;
		}
		(*result)++;
		if (opts->busy < 0)
			rc = busywait(-opts->busy);
		else
			rc = nanosleep(&bwt, NULL);
		if (rc == -1) {
			ctx->ops->lock(ctx->ctx, NAME, false);
			if (errno == EINTR)
				continue;
			fprintf(stderr, "%s(%d): %s in busy\n", __func__, me,
			       strerror(errno));
			return;
		} else
			ctx->ops->lock(ctx->ctx, NAME, false);
		if (nanosleep(&wt, NULL) == -1) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "%s(%d): %s in idle\n", __func__, me,
				strerror(errno));
			return;
		}
	}
	// fprintf(stderr, "aaargh %d\n", me);
}

static void run_workers(struct lock_ctx *ctx, struct options *opts)
{
	int i, wstat;
	struct timespec t1, t2, t3, t4, tw;
	long *results;
	pid_t child,  *pids;
	double sqsum;

	pids = calloc(opts->workers, sizeof(*pids));
	if (pids == NULL)
		return;
	results = mmap(NULL, opts->workers * sizeof(*results),
		       PROT_READ|PROT_WRITE,
		       MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (results == NULL) {
		perror("mmap");
		free(pids);
		return;
	}
	memset(results, 0, sizeof(*results));

	fflush(stdout);
	clock_gettime(CLOCK_REALTIME, &t1);
	for (i = 0; i < opts->workers; i++) {
		child = fork();
		if (child == -1) {
			perror("fork");
			free(pids);
			munmap(results, opts->workers * sizeof(*results));
			return;
		}
		else if (child == 0) {
			struct sigaction sa;

			free(pids);
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = &sig_handler;
			if (sigaction(SIGINT, &sa, NULL) == -1) {
				perror("sigaction");
				exit(1);
			}
			worker(ctx, opts, &results[i]);
			free_lock_ctx(ctx);
			munmap(results, opts->workers * sizeof(*results));
			exit(0);
		}
		else
			pids[i] = child;
	}

	clock_gettime(CLOCK_REALTIME, &t2);
	tw.tv_sec = opts->seconds;
	tw.tv_nsec = 0;
	nanosleep(&tw, NULL);
	clock_gettime(CLOCK_REALTIME, &t3);

	for (i = 0; i < opts->workers; i++) {
		siginfo_t si;
		int r;

		si.si_pid = 0;
		r = waitid(P_PID, pids[i], &si,
			   WEXITED|WNOHANG|WNOWAIT);
		if (r == 0 && si.si_pid == 0) {
//			fprintf(stderr, "shoot %d\n", pids[i]);
			kill(pids[i], SIGINT);
		}
	}

	while (child != -1) {
		child = wait(&wstat);
//		if (child != -1)
//			fprintf(stderr, "%d died\n", child);
	}

	clock_gettime(CLOCK_REALTIME, &t4);
	timespec_subtract(&t2, &t2, &t1);
	timespec_subtract(&t4, &t4, &t3);
	timespec_subtract(&t3, &t3, &t1);

	if ((t4.tv_sec + t4.tv_nsec/1.e9) / (t3.tv_sec + t3.tv_nsec/1.e9)
	    >= 0.05)
		fprintf(stderr,
			"WARNING: run time %ld.%06lds, stop time: %ld.%06lds\n",
			t3.tv_sec, t3.tv_nsec/1000,
			t4.tv_sec, t4.tv_nsec/1000);

	sqsum = opts->total = 0;
	opts->maxi = -1;
	opts->mini = LONG_MAX;
	for (i = 0; i < opts->workers; i++) {
		// fprintf(stderr, "worker %d: %ld\n", i, results[i]);
		if (results[i] > opts->maxi)
			opts->maxi = results[i];
		if (results[i] < opts->mini)
			opts->mini = results[i];
		opts->total += results[i];
		sqsum += results[i] * results[i];
	}
	munmap(results, opts->workers * sizeof(*results));

	opts->avg = (double)opts->total / opts->workers;

	if (opts->workers > 1)
		opts->stddev = sqrt((sqsum -
				     opts->workers * opts->avg * opts->avg)
				    / (opts->workers - 1));
	else
		opts->stddev = 0;
}

enum {
	E_FCNTL = 0,
	E_FLOCK,
	E_SYSV,
	_N_LOCK_TYPES
};

static const struct _lock_type {
	const char *name;
	const struct lock_ops *ops;
} lock_types[_N_LOCK_TYPES] = {
	{ "fcntl", &lock_fcntl_ops },
	{ "flock", &lock_flock_ops },
	{ "sysv", &lock_sysv_ops },
};


static struct lock_ctx *get_ctx(const char *name) {
	struct lock_ctx *ctx;
	int type;

	for (type = E_FCNTL; type < _N_LOCK_TYPES; type++)
		if (!strcmp(name, lock_types[type].name))
			break;

	if (type >= _N_LOCK_TYPES) {
		fprintf(stderr, "%s: bad lock type %s\n", __func__, name);
		return NULL;
	}

	ctx = new_lock_ctx(lock_types[type].ops);
	if (ctx == NULL) {
		perror(__func__);
		return NULL;
	}
	return ctx;
}

static void timediff(const struct tms *end, const struct tms *start,
	      double ticks,
	      double *user, double *sys)
{
	*user = (end->tms_cutime - start->tms_cutime) / ticks;
	*sys = (end->tms_cstime - start->tms_cstime) / ticks;
}

static long run_test(struct options *opts, const char *tst, bool print)
{
	struct tms tm_start, tm_end;
	struct timespec ts_start, ts_end, ts_diff;
	struct lock_ctx *ctx;
	double dt, sys, user;
	double ticks;

	ticks = sysconf(_SC_CLK_TCK);
	if (ticks == -1) {
		perror("_SC_CLK_TC");
		return -1;
	}

	ctx = get_ctx(tst);
	if (ctx == NULL)
		return -1;

	times(&tm_start);
	clock_gettime(CLOCK_REALTIME, &ts_start);

	run_workers(ctx, opts);

	times(&tm_end);
	clock_gettime(CLOCK_REALTIME, &ts_end);
	free_lock_ctx(ctx);

	timespec_subtract(&ts_diff, &ts_end, &ts_start);
	dt = ts_diff.tv_sec + ts_diff.tv_nsec / 1.e9;
	timediff(&tm_end, &tm_start, ticks, &user, &sys);

	if (print)
		printf("%d %.3e %.3e %.3e %ld %ld %ld %.1f %.1f\n",
		       opts->workers,
		       dt, user, sys, opts->total, opts->mini, opts->maxi,
		       opts->avg, opts->stddev);

	return opts->total;
}

static const int def_workers[] = { 10, 20, 50, 100, 200, 500, 1000 };

static int setup_workers(const char *opt, const int **wrk)
{
	int n = 1, i;
	const char *p;
	char *e;
	int *wr;

	if (opt == NULL || *opt == '\0')
		return -1;
	if (!strcmp(opt, "default")) {
		wr = calloc(1, sizeof(def_workers));
		if (!wr)
			return -1;
		memcpy(wr, def_workers, sizeof(def_workers));
		*wrk = wr;
		return sizeof(def_workers)/sizeof(*def_workers);
	}

	for (p = strchr(opt, ':'); p; p = strchr(p + 1, ':'))
		n++;

	wr = calloc(n, sizeof(*wr));
	if (wr == NULL)
		return -1;

	for (p = opt, i = 0; i < n - 1; i++, p = e + 1) {
		if (*p == '\0' || *p == ':')
			goto bad;
		wr[i] = strtol(p, &e, 10);
		if (*e != ':')
			goto bad;
		if (wr[i] <= 0 || (i > 0 && wr[i] <= wr[i - 1]))
			goto bad;
	}
	if (*p == '\0')
		goto bad;
	wr[i] = strtol(p, &e, 10);
	if (*e != '\0')
		goto bad;
	if (wr[i] <= 0 || (i > 0 && wr[i] <= wr[i - 1]))
		goto bad;

	*wrk = wr;
	return n;

bad:
	fprintf(stderr, "bad input for -w: %s\n", opt);
	free(wr);
	return -1;
}

int main(int argc, char *const argv[])
{
	struct options opts = {
		.idle = IDLE,
		.busy = BUSY,
		.seconds = SECONDS,
		.workers = 0,
	};
	int opt, i;
	int nworkers = 0;
	const int *workers = NULL;

	do {
		char *e;

		opt = getopt(argc, argv, "i:b:w:t:");
		switch (opt) {
		case 'i':
			opts.idle = strtol(optarg, &e, 10);
			if (*e != '\0' || opts.idle < 0)
				goto opt_err;
			opts.idle *= 1000;
			break;
		case 'b':
			opts.busy = strtol(optarg, &e, 10);
			if (*e != '\0')
				goto opt_err;
			opts.busy *= 1000;
			break;
		case 't':
			opts.seconds = strtol(optarg, &e, 10);
			if (*e != '\0' || opts.seconds <= 0)
				goto opt_err;
			break;
		case 'w':
			nworkers = setup_workers(optarg, &workers);
			if (workers == NULL)
				goto opt_err;
			break;
		case -1:
			break;
		default:
			goto opt_err;
		}
	} while (opt != -1);

	if (optind != argc - 1)
		goto opt_err;
	if (workers == NULL)
		nworkers = setup_workers("default", &workers);
	if (workers == NULL)
		goto opt_err;

	opts.workers = workers[0];

	printf("\n\n# %s %ld %ld %d\n", argv[optind],
	       opts.busy/1000, opts.idle/1000, opts.seconds);

	for (i = 0; i < nworkers; i++) {
		opts.workers = workers[i];
		run_test(&opts, argv[optind], true);
	}
	free((int*)workers);
	return 0;

opt_err:
	fprintf(stderr,
		"usage: %s -i <idle-us> -b <busy-us> -w <workers> -t <seconds> test\n"
		"   busy-us: time to hold the lock (negative: busy-wait)\n"
		"   idle-us: time to sleep before locking again\n"
		"   workers: ':'-separated list of thread counts, e.g. 10:20:30\n"
		"   seconds:  number of seconds for each test\n",
		argv[0]);
	fprintf(stderr, "available tests: %s", lock_types[0].name);
	for (i = 1; i < _N_LOCK_TYPES; i ++)
		fprintf(stderr, ", %s", lock_types[i].name);
	fprintf(stderr, "\n");
	return 1;
}
