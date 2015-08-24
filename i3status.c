/*
 * vim:ts=8:expandtab
 *
 * i3status – Generates a status line for dzen2 or xmobar
 *
 * Copyright © 2014 Griffin Smith <wildgriffin45 at gmail dot com>
 * Copyright © 2008-2012 Michael Stapelberg and contributors
 * Copyright © 2009 Thorsten Toepper <atsutane at freethoughts dot de>
 * Copyright © 2010 Axel Wagner <mail at merovius dot de>
 * Copyright © 2010 Fernando Tarlá Cardoso Lemos <fernandotcl at gmail dot com>
 *
 * See file LICENSE for license information.
 *
 */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <getopt.h>
#include <signal.h>
#include <confuse.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>

#include <yajl/yajl_gen.h>
#include <yajl/yajl_version.h>
#include <libnotify/notify.h>

#include "i3status.h"

#define exit_if_null(pointer, ...) { if (pointer == NULL) die(__VA_ARGS__); }

#define CFG_COLOR_OPTS(good, degraded, bad) \
    CFG_STR("color_good", good, CFGF_NONE), \
    CFG_STR("color_degraded", degraded, CFGF_NONE), \
    CFG_STR("color_bad", bad, CFGF_NONE)

#define CFG_CUSTOM_COLOR_OPTS CFG_COLOR_OPTS(NULL, NULL, NULL)

/* socket file descriptor for general purposes */
int general_socket;

static bool exit_upon_signal = false;

cfg_t *cfg, *cfg_general, *cfg_section;

/*
 * Set the exit_upon_signal flag, because one cannot do anything in a safe
 * manner in a signal handler (e.g. fprintf, which we really want to do for
 * debugging purposes), see
 * https://www.securecoding.cert.org/confluence/display/seccode/SIG30-C.+Call+only+asynchronous-safe+functions+within+signal+handlers
 *
 */
void fatalsig(int signum) {
        exit_upon_signal = true;
}

/*
 * Do nothing upon SIGUSR1. Running this signal handler will nevertheless
 * interrupt nanosleep() so that i3status immediately generates new output.
 *
 */
void sigusr1(int signum) {
}

/*
 * Checks if the given path exists by calling stat().
 *
 */
static bool path_exists(const char *path) {
        struct stat buf;
        return (stat(path, &buf) == 0);
}

static void *scalloc(size_t size) {
        void *result = calloc(size, 1);
        exit_if_null(result, "Error: out of memory (calloc(%zd))\n", size);
        return result;
}

static char *sstrdup(const char *str) {
        char *result = strdup(str);
        exit_if_null(result, "Error: out of memory (strdup())\n");
        return result;
}


/*
 * Validates a color in "#RRGGBB" format
 *
 */
static int valid_color(const char *value)
{
        if (strlen(value) != 7) return 0;
        if (value[0] != '#') return 0;
        for (int i = 1; i < 7; ++i) {
                if (value[i] >= '0' && value[i] <= '9') continue;
                if (value[i] >= 'a' && value[i] <= 'f') continue;
                if (value[i] >= 'A' && value[i] <= 'F') continue;
                return 0;
        }
        return 1;
}

/*
 * This function resolves ~ in pathnames.
 * It may resolve wildcards in the first part of the path, but if no match
 * or multiple matches are found, it just returns a copy of path as given.
 *
 */
static char *resolve_tilde(const char *path) {
        static glob_t globbuf;
        char *head, *tail, *result = NULL;

        tail = strchr(path, '/');
        head = strndup(path, tail ? (size_t)(tail - path) : strlen(path));

        int res = glob(head, GLOB_TILDE, NULL, &globbuf);
        free(head);
        /* no match, or many wildcard matches are bad */
        if (res == GLOB_NOMATCH || globbuf.gl_pathc != 1)
                result = sstrdup(path);
        else if (res != 0) {
                die("glob() failed");
        } else {
                head = globbuf.gl_pathv[0];
                result = scalloc(strlen(head) + (tail ? strlen(tail) : 0) + 1);
                strncpy(result, head, strlen(head));
                if (tail)
                        strncat(result, tail, strlen(tail));
        }
        globfree(&globbuf);

        return result;
}

static char *get_config_path(void) {
        char *xdg_config_home, *xdg_config_dirs, *config_path;

        /* 1: check the traditional path under the home directory */
        config_path = resolve_tilde("~/.i3status.conf");
        if (path_exists(config_path))
                return config_path;

        /* 2: check for $XDG_CONFIG_HOME/i3status/config */
        if ((xdg_config_home = getenv("XDG_CONFIG_HOME")) == NULL)
                xdg_config_home = "~/.config";

        xdg_config_home = resolve_tilde(xdg_config_home);
        if (asprintf(&config_path, "%s/i3status/config", xdg_config_home) == -1)
                die("asprintf() failed");
        free(xdg_config_home);

        if (path_exists(config_path))
                return config_path;
        free(config_path);

        /* 3: check the traditional path under /etc */
        config_path = SYSCONFDIR "/i3status.conf";
        if (path_exists(config_path))
            return sstrdup(config_path);

        /* 4: check for $XDG_CONFIG_DIRS/i3status/config */
        if ((xdg_config_dirs = getenv("XDG_CONFIG_DIRS")) == NULL)
                xdg_config_dirs = "/etc/xdg";

        char *buf = strdup(xdg_config_dirs);
        char *tok = strtok(buf, ":");
        while (tok != NULL) {
                tok = resolve_tilde(tok);
                if (asprintf(&config_path, "%s/i3status/config", tok) == -1)
                        die("asprintf() failed");
                free(tok);
                if (path_exists(config_path)) {
                        free(buf);
                        return config_path;
                }
                free(config_path);
                tok = strtok(NULL, ":");
        }
        free(buf);

        die("Unable to find the configuration file (looked at "
                "~/.i3status.conf, $XDG_CONFIG_HOME/i3status/config, "
                "/etc/i3status.conf and $XDG_CONFIG_DIRS/i3status/config)");
        return NULL;
}

int main(int argc, char *argv[]) {
        unsigned int j;

        cfg_opt_t general_opts[] = {
                CFG_STR("output_format", "auto", CFGF_NONE),
                CFG_BOOL("colors", 1, CFGF_NONE),
                CFG_STR("color_separator", "#333333", CFGF_NONE),
                CFG_INT("interval", 1, CFGF_NONE),
                CFG_COLOR_OPTS("#00FF00", "#FFFF00", "#FF0000"),
                CFG_END()
        };

        cfg_opt_t mpd_opts[] = {
                CFG_STR("format", "auto", CFGF_NONE),
                CFG_STR("format_stopped", "Stopped", CFGF_NONE),
                CFG_STR("notif_header_format", "%title", CFGF_NONE),
                CFG_STR("notif_body_format", "%artist - %album", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t run_watch_opts[] = {
                CFG_STR("pidfile", NULL, CFGF_NONE),
                CFG_STR("format", "%title: %status", CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t path_exists_opts[] = {
                CFG_STR("path", NULL, CFGF_NONE),
                CFG_STR("format", "%title: %status", CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t wireless_opts[] = {
                CFG_STR("format_up", "W: (%quality at %essid, %bitrate) %ip", CFGF_NONE),
                CFG_STR("format_down", "W: down", CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t ethernet_opts[] = {
                CFG_STR("format_up", "E: %ip (%speed)", CFGF_NONE),
                CFG_STR("format_down", "E: down", CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t ipv6_opts[] = {
                CFG_STR("format_up", "%ip", CFGF_NONE),
                CFG_STR("format_down", "no IPv6", CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t battery_opts[] = {
                CFG_STR("format", "%status %percentage %remaining", CFGF_NONE),
                CFG_STR("format_down", "No battery", CFGF_NONE),
                CFG_STR("notif_header_format", "Battery: %status", CFGF_NONE),
                CFG_STR("notif_body_format", "%percentage, %remaining remaining", CFGF_NONE),
                CFG_STR("path", "/sys/class/power_supply/BAT%d/uevent", CFGF_NONE),
                CFG_INT("low_threshold", 30, CFGF_NONE),
                CFG_INT("critical_threshold", 10, CFGF_NONE),
                CFG_STR("threshold_type", "time", CFGF_NONE),
                CFG_BOOL("last_full_capacity", false, CFGF_NONE),
                CFG_BOOL("integer_battery_capacity", false, CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t time_opts[] = {
                CFG_STR("format", "%Y-%m-%d %H:%M:%S", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t tztime_opts[] = {
                CFG_STR("format", "%Y-%m-%d %H:%M:%S %Z", CFGF_NONE),
                CFG_STR("timezone", "", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t ddate_opts[] = {
                CFG_STR("format", "%{%a, %b %d%}, %Y%N - %H", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t load_opts[] = {
                CFG_STR("format", "%1min %5min %15min", CFGF_NONE),
                CFG_FLOAT("max_threshold", 5, CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t usage_opts[] = {
                CFG_STR("format", "%usage", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t temp_opts[] = {
                CFG_STR("format", "%degrees C", CFGF_NONE),
                CFG_STR("path", NULL, CFGF_NONE),
                CFG_INT("max_threshold", 75, CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t disk_opts[] = {
                CFG_STR("format", "%free", CFGF_NONE),
                CFG_STR("prefix_type", "binary", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t volume_opts[] = {
                CFG_STR("format", "♪: %volume", CFGF_NONE),
                CFG_STR("format_muted", "♪: 0%%", CFGF_NONE),
                CFG_STR("device", "default", CFGF_NONE),
                CFG_STR("mixer", "Master", CFGF_NONE),
                CFG_INT("mixer_idx", 0, CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        cfg_opt_t opts[] = {
                CFG_STR_LIST("order", "{}", CFGF_NONE),
                CFG_SEC("general", general_opts, CFGF_NONE),
                CFG_SEC("mpd", mpd_opts, CFGF_NONE),
                CFG_SEC("run_watch", run_watch_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("path_exists", path_exists_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("wireless", wireless_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("ethernet", ethernet_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("battery", battery_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("cpu_temperature", temp_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("disk", disk_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("volume", volume_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("ipv6", ipv6_opts, CFGF_NONE),
                CFG_SEC("time", time_opts, CFGF_NONE),
                CFG_SEC("tztime", tztime_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("ddate", ddate_opts, CFGF_NONE),
                CFG_SEC("load", load_opts, CFGF_NONE),
                CFG_SEC("cpu_usage", usage_opts, CFGF_NONE),
                CFG_CUSTOM_COLOR_OPTS,
                CFG_END()
        };

        char *configfile = NULL;
        int o, option_index = 0;
        struct option long_options[] = {
                {"config", required_argument, 0, 'c'},
                {"help", no_argument, 0, 'h'},
                {"version", no_argument, 0, 'v'},
                {0, 0, 0, 0}
        };

        struct sigaction action;
        memset(&action, 0, sizeof(struct sigaction));
        action.sa_handler = fatalsig;

        /* Exit upon SIGPIPE because when we have nowhere to write to, gathering system
         * information is pointless. Also exit explicitly on SIGTERM and SIGINT because
         * only this will trigger a reset of the cursor in the terminal output-format.
         */
        sigaction(SIGPIPE, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);

        memset(&action, 0, sizeof(struct sigaction));
        action.sa_handler = sigusr1;
        sigaction(SIGUSR1, &action, NULL);

        if (setlocale(LC_ALL, "") == NULL)
                die("Could not set locale. Please make sure all your LC_* / LANG settings are correct.");

        while ((o = getopt_long(argc, argv, "c:hv", long_options, &option_index)) != -1)
                if ((char)o == 'c')
                        configfile = optarg;
                else if ((char)o == 'h') {
                        printf("i3status " VERSION " © 2008-2012 Michael Stapelberg and contributors\n"
                                "Syntax: %s [-c <configfile>] [-h] [-v]\n", argv[0]);
                        return 0;
                } else if ((char)o == 'v') {
                        printf("i3status " VERSION " © 2008-2012 Michael Stapelberg and contributors\n");
                        return 0;
                }


        if (configfile == NULL)
                configfile = get_config_path();

        cfg = cfg_init(opts, CFGF_NOCASE);
        if (cfg_parse(cfg, configfile) == CFG_PARSE_ERROR)
                return EXIT_FAILURE;

        if (cfg_size(cfg, "order") == 0)
                die("Your 'order' array is empty. Please fix your config.\n");

        cfg_general = cfg_getsec(cfg, "general");
        if (cfg_general == NULL)
                die("Could not get section \"general\"\n");

        char *output_str = cfg_getstr(cfg_general, "output_format");
        if (strcasecmp(output_str, "auto") == 0) {
                fprintf(stderr, "i3status: trying to auto-detect output_format setting\n");
                output_str = auto_detect_format();
                if (!output_str) {
                        output_str = "none";
                        fprintf(stderr, "i3status: falling back to \"none\"\n");
                } else {
                        fprintf(stderr, "i3status: auto-detected \"%s\"\n", output_str);
                }
        }

        if (strcasecmp(output_str, "dzen2") == 0)
                output_format = O_DZEN2;
        else if (strcasecmp(output_str, "xmobar") == 0)
                output_format = O_XMOBAR;
        else if (strcasecmp(output_str, "i3bar") == 0)
                output_format = O_I3BAR;
        else if (strcasecmp(output_str, "term") == 0)
                output_format = O_TERM;
        else if (strcasecmp(output_str, "none") == 0)
                output_format = O_NONE;
        else die("Unknown output format: \"%s\"\n", output_str);

        if (!valid_color(cfg_getstr(cfg_general, "color_good"))
                        || !valid_color(cfg_getstr(cfg_general, "color_degraded"))
                        || !valid_color(cfg_getstr(cfg_general, "color_bad"))
                        || !valid_color(cfg_getstr(cfg_general, "color_separator")))
               die("Bad color format");

#if YAJL_MAJOR >= 2
        yajl_gen json_gen = yajl_gen_alloc(NULL);
#else
        yajl_gen json_gen = yajl_gen_alloc(NULL, NULL);
#endif

        if (output_format == O_I3BAR) {
                /* Initialize the i3bar protocol. See i3/docs/i3bar-protocol
                 * for details. */
                printf("{\"version\":1}\n[\n");
                fflush(stdout);
                yajl_gen_array_open(json_gen);
                yajl_gen_clear(json_gen);
        }
        if (output_format == O_TERM) {
                /* Save the cursor-position and hide the cursor */
                printf("\033[s\033[?25l");
                /* Undo at exit */
                atexit(&reset_cursor);
        }

        if ((general_socket = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
                die("Could not create socket\n");

        // Initialize libnotify
        notify_init("i3status");

        int interval = cfg_getint(cfg_general, "interval");

        /* One memory page which each plugin can use to buffer output.
         * Even though it’s unclean, we just assume that the user will not
         * specify a format string which expands to something longer than 4096
         * bytes — given that the output of i3status is used to display
         * information on screen, more than 1024 characters for the full line
         * (!), not individual plugins, seem very unlikely. */
        char buffer[4096];

        while (1) {
                if (exit_upon_signal) {
                        fprintf(stderr, "Exiting due to signal.\n");
                        cleanup_mpd();
                        notify_uninit();
                        exit(1);
                }
                struct timeval tv;
                gettimeofday(&tv, NULL);
                if (output_format == O_I3BAR)
                        yajl_gen_array_open(json_gen);
                else if (output_format == O_TERM)
                        /* Restore the cursor-position, clear line */
                        printf("\033[u\033[K");
                for (j = 0; j < cfg_size(cfg, "order"); j++) {
                        if (j > 0)
                                print_seperator();

                        const char *current = cfg_getnstr(cfg, "order", j);

                        CASE_SEC("mpd") {
                                SEC_OPEN_MAP("mpd");
                                print_mpd(json_gen, buffer,
                                          cfg_getstr(sec, "format"),
                                          cfg_getstr(sec, "format_stopped"),
                                          cfg_getstr(sec, "notif_header_format"),
                                          cfg_getstr(sec, "notif_body_format"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC("ipv6") {
                                SEC_OPEN_MAP("ipv6");
                                print_ipv6_info(json_gen, buffer, cfg_getstr(sec, "format_up"), cfg_getstr(sec, "format_down"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("wireless") {
                                SEC_OPEN_MAP("wireless");
                                print_wireless_info(json_gen, buffer, title, cfg_getstr(sec, "format_up"), cfg_getstr(sec, "format_down"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("ethernet") {
                                SEC_OPEN_MAP("ethernet");
                                print_eth_info(json_gen, buffer, title, cfg_getstr(sec, "format_up"), cfg_getstr(sec, "format_down"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("battery") {
                                SEC_OPEN_MAP("battery");
                                print_battery_info(json_gen, buffer, atoi(title),
                                        cfg_getstr(sec, "path"),
                                        cfg_getstr(sec, "format"),
                                        cfg_getstr(sec, "format_down"),
                                        cfg_getstr(sec, "notif_header_format"),
                                        cfg_getstr(sec, "notif_body_format"),
                                        cfg_getint(sec, "low_threshold"),
                                        cfg_getstr(sec, "threshold_type"),
                                        cfg_getbool(sec, "last_full_capacity"),
                                        cfg_getbool(sec, "integer_battery_capacity"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("run_watch") {
                                SEC_OPEN_MAP("run_watch");
                                print_run_watch(json_gen, buffer, title, cfg_getstr(sec, "pidfile"), cfg_getstr(sec, "format"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("path_exists") {
                                SEC_OPEN_MAP("path_exists");
                                print_path_exists(json_gen, buffer, title, cfg_getstr(sec, "path"), cfg_getstr(sec, "format"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("disk") {
                                SEC_OPEN_MAP("disk_info");
                                print_disk_info(json_gen, buffer, title, cfg_getstr(sec, "format"), cfg_getstr(sec, "prefix_type"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC("load") {
                                SEC_OPEN_MAP("load");
                                print_load(json_gen, buffer, cfg_getstr(sec, "format"), cfg_getfloat(sec, "max_threshold"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC("time") {
                                SEC_OPEN_MAP("time");
                                print_time(json_gen, buffer, cfg_getstr(sec, "format"), NULL, tv.tv_sec);
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("tztime") {
                                SEC_OPEN_MAP("tztime");
                                print_time(json_gen, buffer, cfg_getstr(sec, "format"), cfg_getstr(sec, "timezone"), tv.tv_sec);
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC("ddate") {
                                SEC_OPEN_MAP("ddate");
                                print_ddate(json_gen, buffer, cfg_getstr(sec, "format"), tv.tv_sec);
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("volume") {
                                SEC_OPEN_MAP("volume");
                                print_volume(json_gen, buffer, cfg_getstr(sec, "format"),
                                             cfg_getstr(sec, "format_muted"),
                                             cfg_getstr(sec, "device"),
                                             cfg_getstr(sec, "mixer"),
                                             cfg_getint(sec, "mixer_idx"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC_TITLE("cpu_temperature") {
                                SEC_OPEN_MAP("cpu_temperature");
                                print_cpu_temperature_info(json_gen, buffer, atoi(title), cfg_getstr(sec, "path"), cfg_getstr(sec, "format"), cfg_getint(sec, "max_threshold"));
                                SEC_CLOSE_MAP;
                        }

                        CASE_SEC("cpu_usage") {
                                SEC_OPEN_MAP("cpu_usage");
                                print_cpu_usage(json_gen, buffer, cfg_getstr(sec, "format"));
                                SEC_CLOSE_MAP;
                        }
                }
                if (output_format == O_I3BAR) {
                        yajl_gen_array_close(json_gen);
                        const unsigned char *buf;
#if YAJL_MAJOR >= 2
                        size_t len;
#else
                        unsigned int len;
#endif
                        yajl_gen_get_buf(json_gen, &buf, &len);
                        write(STDOUT_FILENO, buf, len);
                        yajl_gen_clear(json_gen);
                }

                printf("\n");
                fflush(stdout);

                /* To provide updates on every full second (as good as possible)
                 * we don’t use sleep(interval) but we sleep until the next
                 * second (with microsecond precision) plus (interval-1)
                 * seconds. We also align to 60 seconds modulo interval such
                 * that we start with :00 on every new minute. */
                struct timeval current_timeval;
                gettimeofday(&current_timeval, NULL);
                struct timespec ts = {interval - 1 - (current_timeval.tv_sec % interval), (10e5 - current_timeval.tv_usec) * 1000};
                nanosleep(&ts, NULL);
        }

        cleanup_mpd();
        notify_uninit();
}
