#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define STATE_PATH "/tmp/gpio-motors.state"
#define LOCKDIR "/tmp/gpio-motors.lock"
#define LOCK_PID_PATH "/tmp/gpio-motors.lock/pid"
#define PRESET_DIR "/etc/gpio-motors-presets"
#define CONFIG_PATH "/etc/gpio-motors.conf"

enum { PAN_MIN = 0, PAN_MAX = 615, PAN_CENTER = 307 };
enum { TILT_MIN = 0, TILT_MAX = 180, TILT_CENTER = 140 };

static const int pan_right[4] = {60, 61, 37, 38};
static const int pan_left[4] = {38, 37, 61, 60};
static const int tilt_down[4] = {54, 57, 56, 55};
static const int tilt_up[4] = {55, 56, 57, 54};

static const int half_step[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

enum step_mode {
    STEP_MODE_HALF = 0,
    STEP_MODE_WAVE = 1,
};

struct gpio_line {
    int gpio;
    int fd;
};

static struct gpio_line lines[] = {
    {37, -1}, {38, -1}, {54, -1}, {55, -1},
    {56, -1}, {57, -1}, {60, -1}, {61, -1},
};

static bool have_lock = false;
static int current_x = PAN_CENTER;
static int current_y = TILT_CENTER;
static bool current_position_valid = false;
static int state_steps_since_save = 0;

struct motor_config {
    int speed_delay_us[11];
    int home_sweep_delay_us;
    int home_center_delay_us;
    int ramp_enabled;
    int ramp_min_steps;
    int ramp_max_steps;
    int ramp_slow_multiplier;
    int ramp_min_slow_delay_us;
    int state_save_interval_steps;
    int lock_stale_seconds;
    int pan_motor_invert;
    int tilt_motor_invert;
    enum step_mode step_mode;
};

static struct motor_config cfg = {
    .speed_delay_us = {
        0,
        1800, 1500, 1200, 950, 750,
        550, 400, 250, 120, 60,
    },
    .home_sweep_delay_us = 60,
    .home_center_delay_us = 75,
    .ramp_enabled = 1,
    .ramp_min_steps = 8,
    .ramp_max_steps = 60,
    .ramp_slow_multiplier = 3,
    .ramp_min_slow_delay_us = 500,
    .state_save_interval_steps = 1,
    .lock_stale_seconds = 30,
    .pan_motor_invert = 0,
    .tilt_motor_invert = 0,
    .step_mode = STEP_MODE_HALF,
};

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static int parse_config_int(const char *value, int fallback, int min, int max) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *trim(end) != '\0') return fallback;
    if (parsed < min) return min;
    if (parsed > max) return max;
    return (int)parsed;
}

static void load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);

        int speed = 0;
        if (sscanf(key, "speed%d_delay_us", &speed) == 1 && speed >= 1 && speed <= 10) {
            cfg.speed_delay_us[speed] = parse_config_int(value, cfg.speed_delay_us[speed], 0, 1000000);
        } else if (strcmp(key, "home_sweep_delay_us") == 0) {
            cfg.home_sweep_delay_us = parse_config_int(value, cfg.home_sweep_delay_us, 0, 1000000);
        } else if (strcmp(key, "home_center_delay_us") == 0) {
            cfg.home_center_delay_us = parse_config_int(value, cfg.home_center_delay_us, 0, 1000000);
        } else if (strcmp(key, "ramp_enabled") == 0) {
            cfg.ramp_enabled = parse_config_int(value, cfg.ramp_enabled, 0, 1);
        } else if (strcmp(key, "ramp_min_steps") == 0) {
            cfg.ramp_min_steps = parse_config_int(value, cfg.ramp_min_steps, 0, 10000);
        } else if (strcmp(key, "ramp_max_steps") == 0) {
            cfg.ramp_max_steps = parse_config_int(value, cfg.ramp_max_steps, 0, 10000);
        } else if (strcmp(key, "ramp_slow_multiplier") == 0) {
            cfg.ramp_slow_multiplier = parse_config_int(value, cfg.ramp_slow_multiplier, 1, 100);
        } else if (strcmp(key, "ramp_min_slow_delay_us") == 0) {
            cfg.ramp_min_slow_delay_us = parse_config_int(value, cfg.ramp_min_slow_delay_us, 0, 1000000);
        } else if (strcmp(key, "state_save_interval_steps") == 0) {
            cfg.state_save_interval_steps = parse_config_int(value, cfg.state_save_interval_steps, 0, 10000);
        } else if (strcmp(key, "lock_stale_seconds") == 0) {
            cfg.lock_stale_seconds = parse_config_int(value, cfg.lock_stale_seconds, 1, 86400);
        } else if (strcmp(key, "pan_motor_invert") == 0) {
            cfg.pan_motor_invert = parse_config_int(value, cfg.pan_motor_invert, 0, 1);
        } else if (strcmp(key, "tilt_motor_invert") == 0) {
            cfg.tilt_motor_invert = parse_config_int(value, cfg.tilt_motor_invert, 0, 1);
        } else if (strcmp(key, "step_mode") == 0) {
            if (strcasecmp(value, "wave") == 0 || strcasecmp(value, "script") == 0) {
                cfg.step_mode = STEP_MODE_WAVE;
            } else if (strcasecmp(value, "half") == 0 || strcasecmp(value, "halfstep") == 0) {
                cfg.step_mode = STEP_MODE_HALF;
            }
        }
    }

    fclose(f);
}

static void sleep_us(int usec) {
    if (usec <= 0) return;
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (long)(usec % 1000000) * 1000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static int delay_for_speed(int speed) {
    if (speed < 1 || speed > 10) speed = 5;
    return cfg.speed_delay_us[speed];
}

static void usage(void) {
    puts("Usage:");
    puts("  gpio-motors X Y SPEED");
    puts("  gpio-motors --home  # calibrate, then return to parking preset if saved");
    puts("  gpio-motors --center [SPEED]");
    puts("  gpio-motors --status");
    puts("  gpio-motors --absolute X Y [SPEED]");
    puts("  gpio-motors --preset-set N|parking");
    puts("  gpio-motors --preset-go N|parking [SPEED]");
    puts("  gpio-motors --off");
    puts("");
    puts("Tuning file: /etc/gpio-motors.conf");
}

static int write_all(int fd, const char *value) {
    size_t len = strlen(value);
    while (len > 0) {
        ssize_t wr = write(fd, value, len);
        if (wr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        value += wr;
        len -= (size_t)wr;
    }
    return 0;
}

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(value);
    ssize_t wr = write(fd, value, len);
    close(fd);
    return wr == len ? 0 : -1;
}

static int export_gpio(int gpio) {
    char path[128], value[32];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
    if (access(path, F_OK) != 0) {
        snprintf(value, sizeof(value), "%d", gpio);
        int fd = open("/sys/class/gpio/export", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            (void)write_all(fd, value);
            close(fd);
        }
    }
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    write_file(path, "out");
    return 0;
}

static struct gpio_line *find_line(int gpio) {
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if (lines[i].gpio == gpio) return &lines[i];
    }
    return NULL;
}

static int init_gpios(void) {
    char path[128];
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        export_gpio(lines[i].gpio);
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", lines[i].gpio);
        lines[i].fd = open(path, O_WRONLY | O_CLOEXEC);
        if (lines[i].fd < 0) {
            fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static void set_gpio(int gpio, int value) {
    struct gpio_line *line = find_line(gpio);
    if (!line || line->fd < 0) return;
    const char value_text[2] = {value ? '1' : '0', '\0'};
    lseek(line->fd, 0, SEEK_SET);
    (void)write_all(line->fd, value_text);
}

static void all_off(void) {
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        set_gpio(lines[i].gpio, 0);
    }
}

static void step_axis(const int pins[4], int delay_us) {
    if (cfg.step_mode == STEP_MODE_WAVE) {
        for (int phase = 0; phase < 4; phase++) {
            set_gpio(pins[phase], 1);
            sleep_us(delay_us);
            set_gpio(pins[phase], 0);
        }
        return;
    }

    for (int phase = 0; phase < 8; phase++) {
        for (int i = 0; i < 4; i++) {
            set_gpio(pins[i], half_step[phase][i]);
        }
        sleep_us(delay_us);
    }
}

static void lock_release(void) {
    if (have_lock) {
        all_off();
        unlink(LOCK_PID_PATH);
        rmdir(LOCKDIR);
        have_lock = false;
    }
}

static void on_signal(int sig) {
    (void)sig;
    if (current_position_valid) {
        char tmp_path[PATH_MAX];
        char data[64];
        snprintf(tmp_path, sizeof(tmp_path), "%s.%ld", STATE_PATH, (long)getpid());
        int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            snprintf(data, sizeof(data), "X=%d\nY=%d\n", current_x, current_y);
            if (write_all(fd, data) == 0) {
                fsync(fd);
                close(fd);
                rename(tmp_path, STATE_PATH);
            } else {
                close(fd);
                unlink(tmp_path);
            }
        }
    }
    lock_release();
    _exit(128 + sig);
}

static bool pid_is_alive(pid_t pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

static bool lock_is_stale(void) {
    FILE *f = fopen(LOCK_PID_PATH, "r");
    if (f) {
        long pid = -1;
        int matched = fscanf(f, "%ld", &pid);
        fclose(f);
        if (matched == 1) return !pid_is_alive((pid_t)pid);
    }

    struct stat st;
    if (stat(LOCKDIR, &st) != 0) return false;
    return time(NULL) - st.st_mtime > cfg.lock_stale_seconds;
}

static void remove_stale_lock(void) {
    if (!lock_is_stale()) return;
    unlink(LOCK_PID_PATH);
    rmdir(LOCKDIR);
}

static int lock_acquire(void) {
    for (int i = 0; i < 200; i++) {
        if (mkdir(LOCKDIR, 0755) == 0) {
            char pid[32];
            int fd;
            have_lock = true;
            snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
            fd = open(LOCK_PID_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                write_all(fd, pid);
                close(fd);
            }
            return 0;
        }
        remove_stale_lock();
        sleep_us(50000);
    }
    fprintf(stderr, "gpio-motors busy\n");
    return -1;
}

static int clamp(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static void load_state(int *x, int *y) {
    FILE *f = fopen(STATE_PATH, "r");
    *x = PAN_CENTER;
    *y = TILT_CENTER;
    if (!f) return;

    char key[16];
    int val;
    while (fscanf(f, "%15[^=]=%d\n", key, &val) == 2) {
        if (strcmp(key, "X") == 0) *x = val;
        if (strcmp(key, "Y") == 0) *y = val;
    }
    fclose(f);
    *x = clamp(*x, PAN_MIN, PAN_MAX);
    *y = clamp(*y, TILT_MIN, TILT_MAX);
}

static void save_state(int x, int y) {
    char tmp_path[PATH_MAX];
    char data[64];
    int fd;

    snprintf(tmp_path, sizeof(tmp_path), "%s.%ld", STATE_PATH, (long)getpid());
    snprintf(data, sizeof(data), "X=%d\nY=%d\n", x, y);
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write_all(fd, data) != 0) {
        close(fd);
        unlink(tmp_path);
        return;
    }
    fsync(fd);
    close(fd);
    rename(tmp_path, STATE_PATH);
    state_steps_since_save = 0;
}

static void note_position(int x, int y, bool force) {
    current_x = clamp(x, PAN_MIN, PAN_MAX);
    current_y = clamp(y, TILT_MIN, TILT_MAX);
    current_position_valid = true;
    state_steps_since_save++;

    if (force || cfg.state_save_interval_steps <= 1 ||
        state_steps_since_save >= cfg.state_save_interval_steps) {
        save_state(current_x, current_y);
    }
}

static bool moving(void) {
    struct stat st;
    if (stat(LOCKDIR, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (lock_is_stale()) return false;
    return true;
}

static void status(void) {
    int x, y;
    load_state(&x, &y);
    printf("{\"x\":%d,\"y\":%d,\"pan_min\":%d,\"pan_max\":%d,\"tilt_min\":%d,\"tilt_max\":%d,\"moving\":%s}\n",
           x, y, PAN_MIN, PAN_MAX, TILT_MIN, TILT_MAX, moving() ? "true" : "false");
}

static const int *pan_dir_for_delta(int dx) {
    bool positive = dx >= 0;
    if (cfg.pan_motor_invert) positive = !positive;
    return positive ? pan_right : pan_left;
}

static const int *tilt_dir_for_delta(int dy) {
    bool positive = dy >= 0;
    if (cfg.tilt_motor_invert) positive = !positive;
    return positive ? tilt_down : tilt_up;
}

static const char *resolve_preset_name(const char *name) {
    if (!name || !*name) return "";
    if (strcasecmp(name, "parking") == 0) return "1";
    return name;
}

static const char *preset_display_name(const char *name) {
    if (!name) return "";
    if (strcmp(name, "1") == 0 || strcasecmp(name, "parking") == 0) return "parking";
    return name;
}

static int ramp_delay(int base_delay, int step, int total) {
    if (!cfg.ramp_enabled || total < cfg.ramp_min_steps) return base_delay;
    int ramp = total / 6;
    if (ramp < cfg.ramp_min_steps) ramp = cfg.ramp_min_steps;
    if (ramp > cfg.ramp_max_steps) ramp = cfg.ramp_max_steps;

    int edge = step < total - step ? step : total - step;
    if (edge >= ramp) return base_delay;

    int slow = base_delay * cfg.ramp_slow_multiplier;
    if (slow < cfg.ramp_min_slow_delay_us) slow = cfg.ramp_min_slow_delay_us;
    return base_delay + ((slow - base_delay) * (ramp - edge)) / ramp;
}

static int move_relative(int dx, int dy, int speed) {
    int x, y;
    int target_x, target_y;
    int pan_steps, tilt_steps, total;
    int pan_err = 0, tilt_err = 0;

    if (init_gpios() != 0) return 1;
    if (lock_acquire() != 0) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    load_state(&x, &y);
    note_position(x, y, true);
    target_x = clamp(x + dx, PAN_MIN, PAN_MAX);
    target_y = clamp(y + dy, TILT_MIN, TILT_MAX);
    dx = target_x - x;
    dy = target_y - y;

    pan_steps = abs(dx);
    tilt_steps = abs(dy);
    total = pan_steps > tilt_steps ? pan_steps : tilt_steps;

    const int *pan_dir = pan_dir_for_delta(dx);
    const int *tilt_dir = tilt_dir_for_delta(dy);
    int delay = delay_for_speed(speed);

    for (int step = 0; step < total; step++) {
        int this_delay = ramp_delay(delay, step, total);
        pan_err += pan_steps;
        tilt_err += tilt_steps;

        if (pan_err >= total && pan_steps > 0) {
            step_axis(pan_dir, this_delay);
            x += dx >= 0 ? 1 : -1;
            note_position(x, y, false);
            pan_err -= total;
        }
        if (tilt_err >= total && tilt_steps > 0) {
            step_axis(tilt_dir, this_delay);
            y += dy >= 0 ? 1 : -1;
            note_position(x, y, false);
            tilt_err -= total;
        }
    }

    x = clamp(x, PAN_MIN, PAN_MAX);
    y = clamp(y, TILT_MIN, TILT_MAX);
    note_position(x, y, true);
    lock_release();
    status();
    return 0;
}

static void move_locked_from_to(int *x, int *y, int target_x, int target_y, int delay) {
    int dx = clamp(target_x, PAN_MIN, PAN_MAX) - *x;
    int dy = clamp(target_y, TILT_MIN, TILT_MAX) - *y;
    int pan_steps = abs(dx);
    int tilt_steps = abs(dy);
    int total = pan_steps > tilt_steps ? pan_steps : tilt_steps;
    int pan_err = 0, tilt_err = 0;

    if (total <= 0) return;

    const int *pan_dir = pan_dir_for_delta(dx);
    const int *tilt_dir = tilt_dir_for_delta(dy);

    for (int step = 0; step < total; step++) {
        int this_delay = ramp_delay(delay, step, total);
        pan_err += pan_steps;
        tilt_err += tilt_steps;

        if (pan_err >= total && pan_steps > 0) {
            step_axis(pan_dir, this_delay);
            *x += dx >= 0 ? 1 : -1;
            note_position(*x, *y, false);
            pan_err -= total;
        }
        if (tilt_err >= total && tilt_steps > 0) {
            step_axis(tilt_dir, this_delay);
            *y += dy >= 0 ? 1 : -1;
            note_position(*x, *y, false);
            tilt_err -= total;
        }
    }
}

static int read_preset_xy(const char *arg, int *x, int *y) {
    const char *name = resolve_preset_name(arg);
    char path[PATH_MAX];
    char key[16];
    int val;
    bool have_x = false, have_y = false;

    if (!*name) return 0;
    snprintf(path, sizeof(path), "%s/%s", PRESET_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    while (fscanf(f, "%15[^=]=%d\n", key, &val) == 2) {
        if (strcmp(key, "X") == 0) {
            *x = clamp(val, PAN_MIN, PAN_MAX);
            have_x = true;
        }
        if (strcmp(key, "Y") == 0) {
            *y = clamp(val, TILT_MIN, TILT_MAX);
            have_y = true;
        }
    }
    fclose(f);
    return have_x && have_y;
}

static int go_absolute(int target_x, int target_y, int speed) {
    int x, y;
    load_state(&x, &y);
    return move_relative(target_x - x, target_y - y, speed);
}

static int home(void) {
    int x = 0, y = 0;
    if (init_gpios() != 0) return 1;
    if (lock_acquire() != 0) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    for (int i = 0; i < 700; i++) step_axis(pan_dir_for_delta(-1), cfg.home_sweep_delay_us);
    for (int i = 0; i < 220; i++) step_axis(tilt_dir_for_delta(-1), cfg.home_sweep_delay_us);
    note_position(0, 0, true);

    for (int i = 0; i < PAN_CENTER; i++) {
        step_axis(pan_dir_for_delta(1), ramp_delay(cfg.home_center_delay_us, i, PAN_CENTER));
        x++;
        note_position(x, y, false);
    }
    for (int i = 0; i < TILT_CENTER; i++) {
        step_axis(tilt_dir_for_delta(1), ramp_delay(cfg.home_center_delay_us, i, TILT_CENTER));
        y++;
        note_position(x, y, false);
    }

    int parking_x = x, parking_y = y;
    if (read_preset_xy("parking", &parking_x, &parking_y)) {
        move_locked_from_to(&x, &y, parking_x, parking_y, delay_for_speed(10));
    }

    note_position(x, y, true);
    lock_release();
    status();
    return 0;
}

static int preset_set(const char *arg) {
    const char *name = resolve_preset_name(arg);
    char path[PATH_MAX];
    int x, y;
    if (!*name) {
        fprintf(stderr, "missing preset number\n");
        return 1;
    }
    mkdir(PRESET_DIR, 0755);
    load_state(&x, &y);
    snprintf(path, sizeof(path), "%s/%s", PRESET_DIR, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "failed to write %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(f, "X=%d\nY=%d\n", x, y);
    fclose(f);
    printf("saved preset %s at X=%d Y=%d\n", preset_display_name(name), x, y);
    return 0;
}

static int preset_go(const char *arg, int speed) {
    int x = PAN_CENTER, y = TILT_CENTER;
    if (!*resolve_preset_name(arg)) {
        fprintf(stderr, "missing preset number\n");
        return 1;
    }
    if (!read_preset_xy(arg, &x, &y)) {
        fprintf(stderr, "preset %s not found\n", preset_display_name(resolve_preset_name(arg)));
        return 1;
    }
    return go_absolute(x, y, speed);
}

static int parse_int(const char *s, int def) {
    if (!s) return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end) return def;
    if (v < INT_MIN) return INT_MIN;
    if (v > INT_MAX) return INT_MAX;
    return (int)v;
}

int main(int argc, char **argv) {
    load_config();

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--status") == 0) {
        status();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--off") == 0) {
        if (init_gpios() != 0) return 1;
        all_off();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--home") == 0) return home();
    if (argc > 1 && strcmp(argv[1], "--center") == 0) {
        return go_absolute(PAN_CENTER, TILT_CENTER, parse_int(argc > 2 ? argv[2] : NULL, 10));
    }
    if (argc > 1 && strcmp(argv[1], "--preset-set") == 0) {
        return preset_set(argc > 2 ? argv[2] : NULL);
    }
    if (argc > 1 && strcmp(argv[1], "--preset-go") == 0) {
        return preset_go(argc > 2 ? argv[2] : NULL, parse_int(argc > 3 ? argv[3] : NULL, 10));
    }
    if (argc > 1 && strcmp(argv[1], "--absolute") == 0) {
        return go_absolute(parse_int(argc > 2 ? argv[2] : NULL, PAN_CENTER),
                           parse_int(argc > 3 ? argv[3] : NULL, TILT_CENTER),
                           parse_int(argc > 4 ? argv[4] : NULL, 10));
    }
    return move_relative(parse_int(argc > 1 ? argv[1] : NULL, 0),
                         parse_int(argc > 2 ? argv[2] : NULL, 0),
                         parse_int(argc > 3 ? argv[3] : NULL, 10));
}
