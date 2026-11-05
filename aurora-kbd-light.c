// SPDX-License-Identifier: MIT
/*
 * aurora-kbd-light: drive the keyboard backlight from the ambient light
 * sensor, as macOS does.
 *
 * The level follows the room: brightest in the dark, fading out as the room
 * gets brighter, off in daylight. A change made by anyone else (the desktop's
 * slider, idle dimming) is respected: setting it to zero keeps it off until it
 * is turned on again, and any other level holds until the room's light
 * changes a lot. With the lid closed the backlight is off.
 *
 * Only sysfs and evdev are used, so this needs nothing beyond libc.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS 250		/* sensor poll while steady */
#define FADE_TICK_MS 40		/* LED steps while fading */
#define FADE_MS 600		/* duration of a full-range fade */
#define SMOOTH_S 2.0		/* time constant of the lux average */
#define DEADBAND 3		/* ignore smaller level changes */
#define HOLD_RATIO 4.0		/* light change that ends a manual hold */

struct config {
	double full_lux;	/* at or below: the brightest automatic level */
	double on_lux;		/* below: the backlight comes on */
	double off_lux;		/* above: it goes off again */
	int min_pct;		/* level just after it comes on */
	int max_pct;		/* level in the dark */
	bool verbose;
};

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double env_double(const char *name, double def)
{
	const char *v = getenv(name);
	char *end;
	double d;

	if (!v || !*v)
		return def;
	d = strtod(v, &end);
	return (*end || d < 0) ? def : d;
}

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	return 0;
}

static int read_int(const char *path, long *out)
{
	char buf[64], *end;
	int ret = read_file(path, buf, sizeof(buf));

	if (ret)
		return ret;
	*out = strtol(buf, &end, 10);
	return end == buf ? -EINVAL : 0;
}

static int write_int(const char *path, long value)
{
	char buf[32];
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int len, ret = 0;

	if (fd < 0)
		return -errno;
	len = snprintf(buf, sizeof(buf), "%ld\n", value);
	if (write(fd, buf, len) != len)
		ret = -errno;
	close(fd);
	return ret;
}

/* The first LED class device named like a keyboard backlight. */
static char *find_led(void)
{
	glob_t g;
	char *path = NULL;

	if (glob("/sys/class/leds/*kbd_backlight", 0, NULL, &g) == 0 && g.gl_pathc)
		path = strdup(g.gl_pathv[0]);
	globfree(&g);
	return path;
}

/* The first IIO device with a processed illuminance channel. */
static char *find_als(void)
{
	glob_t g;
	char *path = NULL;

	if (glob("/sys/bus/iio/devices/iio:device*/in_illuminance_input", 0, NULL, &g) == 0 &&
	    g.gl_pathc)
		path = strdup(g.gl_pathv[0]);
	globfree(&g);
	return path;
}

static int read_lux(const char *path, double *lux)
{
	char buf[64], *end;
	int ret = read_file(path, buf, sizeof(buf));

	if (ret)
		return ret;
	*lux = strtod(buf, &end);
	return (end == buf || *lux < 0) ? -EINVAL : 0;
}

/* An event device reporting the lid switch, or -1. */
static int open_lid(void)
{
	unsigned long bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	struct dirent *de;
	DIR *dir = opendir("/dev/input");
	int fd = -1;

	if (!dir)
		return -1;
	while ((de = readdir(dir)) && fd < 0) {
		char path[300];
		int cand;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		cand = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (cand < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(cand, EVIOCGBIT(EV_SW, sizeof(bits)), bits) >= 0 &&
		    (bits[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1)
			fd = cand;
		else
			close(cand);
	}
	closedir(dir);
	return fd;
}

static bool lid_closed(int fd)
{
	unsigned long bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };

	if (fd < 0 || ioctl(fd, EVIOCGSW(sizeof(bits)), bits) < 0)
		return false;
	return (bits[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Automatic level for a smoothed log10(lux), with on/off hysteresis. */
static long auto_level(const struct config *c, double log_lux, bool lit, long max)
{
	double lo = log10(c->full_lux), hi = log10(c->on_lux), frac, pct;

	if (log_lux >= log10(lit ? c->off_lux : c->on_lux))
		return 0;
	frac = (hi - log_lux) / (hi - lo);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	pct = c->min_pct + (c->max_pct - c->min_pct) * frac;
	return lround(max * pct / 100.0);
}

int main(int argc, char **argv)
{
	struct config c = {
		.full_lux = env_double("AURORA_KBD_LIGHT_FULL_LUX", 2),
		.on_lux = env_double("AURORA_KBD_LIGHT_ON_LUX", 60),
		.off_lux = env_double("AURORA_KBD_LIGHT_OFF_LUX", 120),
		.min_pct = (int)env_double("AURORA_KBD_LIGHT_MIN_PERCENT", 10),
		.max_pct = (int)env_double("AURORA_KBD_LIGHT_MAX_PERCENT", 50),
	};
	char *led_dir, *als_path, bright_path[512], max_path[512];
	double log_lux = NAN, last_t, hold_log_lux = 0;
	long max, cur, written = -1, auto_last = -1, target;
	bool hold = false, off_latch = false, write_failed = false;
	double lid_retry_t;
	int lid_fd;

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			c.verbose = true;
	if (c.max_pct > 100)
		c.max_pct = 100;
	if (c.min_pct > c.max_pct)
		c.min_pct = c.max_pct;
	if (!(c.full_lux > 0 && c.full_lux < c.on_lux && c.on_lux <= c.off_lux)) {
		fprintf(stderr, "invalid lux thresholds\n");
		return 2;
	}

	led_dir = find_led();
	als_path = find_als();
	if (!led_dir || !als_path) {
		fprintf(stderr, "no %s; nothing to do\n",
			led_dir ? "ambient light sensor" : "keyboard backlight");
		return 0;
	}
	snprintf(bright_path, sizeof(bright_path), "%s/brightness", led_dir);
	snprintf(max_path, sizeof(max_path), "%s/max_brightness", led_dir);
	if (read_int(max_path, &max) || max <= 0) {
		fprintf(stderr, "cannot read %s\n", max_path);
		return 1;
	}
	lid_fd = open_lid();
	fprintf(stderr, "keyboard backlight %s (max %ld), light sensor %s, lid switch %s\n",
		led_dir, max, als_path, lid_fd >= 0 ? "found" : "not found");

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	last_t = lid_retry_t = now_s();

	while (!stop) {
		double lux, t = now_s(), dt = t - last_t;
		bool fading;

		last_t = t;
		/* The lid switch can register after this starts. */
		if (lid_fd < 0 && t - lid_retry_t > 30) {
			lid_retry_t = t;
			lid_fd = open_lid();
		}
		if (read_int(bright_path, &cur)) {
			sleep_ms(TICK_MS);
			continue;
		}

		/* Someone else set the level: respect it. */
		if (written >= 0 && cur != written) {
			if (cur == 0) {
				off_latch = true;
				hold = false;
			} else if (cur == auto_last) {
				off_latch = hold = false;	/* restored after idle dimming */
			} else {
				off_latch = false;
				hold = true;
				hold_log_lux = log_lux;
			}
			if (c.verbose)
				fprintf(stderr, "external change to %ld: %s\n", cur,
					off_latch ? "off until turned on" :
					hold ? "holding" : "resuming");
			written = cur;
		}

		if (read_lux(als_path, &lux) == 0) {
			double l = log10(lux + 0.01);

			log_lux = isnan(log_lux) ? l :
				  log_lux + (l - log_lux) * (dt / (SMOOTH_S + dt));
		}
		if (hold && !isnan(log_lux) && fabs(log_lux - hold_log_lux) > log10(HOLD_RATIO))
			hold = false;

		if (written < 0)
			written = cur;	/* first pass: start from what is set */
		if (off_latch || hold || isnan(log_lux)) {
			sleep_ms(TICK_MS);
			continue;
		}

		target = lid_closed(lid_fd) ? 0 : auto_level(&c, log_lux, written > 0, max);
		if (labs(target - written) < DEADBAND && target != 0 && written != 0)
			target = written;
		fading = target != written;
		if (fading) {
			long step = max * FADE_TICK_MS / FADE_MS;
			long next;

			if (step < 1)
				step = 1;
			next = target > written ? written + step : written - step;
			if ((target > written && next > target) || (target < written && next < target))
				next = target;
			if (write_int(bright_path, next) == 0) {
				written = next;
				if (written == target)
					auto_last = target;
				write_failed = false;
			} else {
				if (!write_failed)
					fprintf(stderr, "cannot write %s: %s\n", bright_path,
						strerror(errno));
				write_failed = true;
				fading = false;	/* retry at the normal pace */
			}
			if (c.verbose && written == target)
				fprintf(stderr, "lux %.2f -> level %ld\n", pow(10, log_lux), target);
		} else {
			auto_last = written;
		}
		sleep_ms(fading ? FADE_TICK_MS : TICK_MS);
	}

	if (lid_fd >= 0)
		close(lid_fd);
	return 0;
}
