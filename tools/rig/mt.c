/* mt: a virtual multi-touch screen on the phone (/dev/uinput), driven from stdin, for gestures adb's input cannot make.
 *
 *   mt WIDTH HEIGHT < script      the axes in display pixels (wm size)
 *
 * Script lines: "d ID X Y" finger ID down, "m ID X Y" move, "u ID" lift (each followed by a report), "w MS" wait.
 * BUS_VIRTUAL and INPUT_PROP_DIRECT: Android takes it for an internal touch screen on the default display. */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void emit(int type, int code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("write");
}

static void wait_ms(int ms)
{
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

static void axis(int code, int max)
{
	struct uinput_abs_setup abs;
	memset(&abs, 0, sizeof(abs));
	abs.code = code;
	abs.absinfo.maximum = max;
	if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
		perror("UI_ABS_SETUP");
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: mt WIDTH HEIGHT < script\n");
		return 2;
	}
	int width = atoi(argv[1]), height = atoi(argv[2]);
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/uinput");
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	axis(ABS_MT_SLOT, 9);
	axis(ABS_MT_TRACKING_ID, 65535);
	axis(ABS_MT_POSITION_X, width - 1);
	axis(ABS_MT_POSITION_Y, height - 1);
	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1af4;
	setup.id.product = 0x7701;
	strcpy(setup.name, "rig-multitouch");
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("UI_DEV_CREATE");
		return 1;
	}
	wait_ms(1500); /* the input reader opens the new device */
	char line[128];
	int down = 0, tracking = 100;
	while (fgets(line, sizeof(line), stdin)) {
		int id, x, y, ms;
		if (sscanf(line, "d %d %d %d", &id, &x, &y) == 3) {
			emit(EV_ABS, ABS_MT_SLOT, id);
			emit(EV_ABS, ABS_MT_TRACKING_ID, tracking++);
			emit(EV_ABS, ABS_MT_POSITION_X, x);
			emit(EV_ABS, ABS_MT_POSITION_Y, y);
			if (down++ == 0)
				emit(EV_KEY, BTN_TOUCH, 1);
			emit(EV_SYN, SYN_REPORT, 0);
		} else if (sscanf(line, "m %d %d %d", &id, &x, &y) == 3) {
			emit(EV_ABS, ABS_MT_SLOT, id);
			emit(EV_ABS, ABS_MT_POSITION_X, x);
			emit(EV_ABS, ABS_MT_POSITION_Y, y);
			emit(EV_SYN, SYN_REPORT, 0);
		} else if (sscanf(line, "u %d", &id) == 1) {
			emit(EV_ABS, ABS_MT_SLOT, id);
			emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
			if (--down == 0)
				emit(EV_KEY, BTN_TOUCH, 0);
			emit(EV_SYN, SYN_REPORT, 0);
		} else if (sscanf(line, "w %d", &ms) == 1) {
			wait_ms(ms);
		}
	}
	wait_ms(300);
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}
