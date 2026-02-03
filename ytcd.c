#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The prefix of the extractor arg that provides the URL of the POT provider server.
#define BGUTIL_YTDLP_POT_PROVIDER_EXTRACTOR_ARGS_PREFIX "youtubepot-bgutilhttp:base_url="

// The longest valid URL of a POT provider server. It's going to be something small like "http://pot-provider:4416" if
// using Docker's container name DNS, but 256 seems like it should cover all practical URLs.
#define MAX_POT_PROVIDER_URL_LEN 256

// The longest valid YouTube channel name.
#define MAX_YOUTUBE_CHANNEL_NAME_LEN 64

// A buffer big enough to hold "upload_date >= YYYYMMDD".
static char break_match_filters_arg[24];

// A buffer big enough to hold the string "<url>/ping".
static char bgutil_ytdlp_pot_provider_ping_url[MAX_POT_PROVIDER_URL_LEN + sizeof "/ping"];

// A buffer big enough to hold the string "youtubepot-bgutilhttp:base_url=<url>".
static char bgutil_ytdlp_pot_provider_extractor_args[
    MAX_POT_PROVIDER_URL_LEN + sizeof BGUTIL_YTDLP_POT_PROVIDER_EXTRACTOR_ARGS_PREFIX];

// A buffer big enough to hold the longest valid channel name, which is 64 characters, plus room for a trailing newline
// (since we're reading these out of a file with fgets), and the null terminator byte.
static char channel_name[MAX_YOUTUBE_CHANNEL_NAME_LEN + 2];

// The video feed URLs we download look like "https://www.youtube.com/@<channel-name>/videos". So if <channel-name> is
// at most 64 characters, then the whole URL is at most that plus the length of "https://www.youtube.com/@/videos".
// (sizeof includes the null byte at the end of a string literal, which we want).
static char channel_videos_url[MAX_YOUTUBE_CHANNEL_NAME_LEN + sizeof "https://www.youtube.com/@/videos"];

// A buffer big enough to hold "YYYYMMDD".
static char datebefore_arg[9];

// =====================================================================================================================
// Trivial wrappers. All `ytcd_X` simply wrap some `X` in the stdlib or other library, and handle errors by printing an
// error message and exiting the process. The purpose of these wrappers is to improve the readability of main logic of
// the program.

static void ytcd_curl_global_init(long flags) {
    CURLcode code = curl_global_init(flags);
    if (code != 0) {
        fprintf(stderr, "[ytcd] FATAL: curl_global_init() failed: %d\n", code);
        exit(1);
    }
}

static CURL* ytcd_curl_easy_init() {
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "[ytcd] FATAL: curl_easy_init() failed\n");
        curl_global_cleanup(); // we assume curl_global_init() was called
        exit(1);
    }
    return curl;
}

static void ytcd_execvp(const char* file, char* const argv[]) {
    execvp(file, argv);
    perror("[ytcd] FATAL: execvp");
    _exit(127);
}

static FILE* ytcd_fopen(const char* restrict path, const char* restrict mode) {
    FILE* file = fopen(path, mode);
    if (file == NULL) {
        fprintf(stderr, "[ytcd] FATAL: Couldn't open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    return file;
}

static pid_t ytcd_fork() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[ytcd] FATAL: fork");
        exit(1);
    }
    return pid;
}

// getenv(), but asserts that the environment variable exists and is non-empty.
static char* ytcd_getenv(const char *name) {
    char* value = getenv(name);
    if (value == NULL || *value == 0) {
        fprintf(stderr, "[ytcd] FATAL: Environment variable %s must be set\n", name);
        exit(1);
    }
    return value;
}

static void ytcd_localtime(const time_t* clock, struct tm* result) {
    if (localtime_r(clock, result) == NULL) {
        perror("[ytcd] FATAL: localtime_r");
        exit(1);
    }
}

static time_t ytcd_mktime(struct tm* timeptr) {
    time_t t = mktime(timeptr);
    if (t == (time_t) -1) {
        perror("[ytcd] FATAL: mktime");
        exit(1);
    }
    return t;
}

static time_t ytcd_time() {
    time_t now = time(NULL);
    if (now == (time_t) -1) {
        perror("[ytcd] FATAL: time");
        exit(1);
    }
    return now;
}

// =====================================================================================================================
// Random helpers and utilities

// Return whether or not a YouTube channel name is invalid. Precondition: it's not empty, and not too long.
static bool is_invalid_youtube_channel_name(const char* channel_name, const size_t channel_name_len) {
    for (size_t i = 0; i < channel_name_len; i++) {
        char c = channel_name[i];
        int ok = (c >= 'a' && c <= 'z')
                    || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9')
                    || c == '.'
                    || c == '_'
                    || c == '-';
        if (!ok) {
            return true;
        }
    }
    return false;
}

// Subtract a number of days from a date.
static struct tm subtract_days(struct tm date, int days) {
    struct tm earlier_date = date;
    earlier_date.tm_mday -= days;
    earlier_date.tm_isdst = -1; // let mktime determine DST
    ytcd_mktime(&earlier_date); // ignore returned time_t, just calling for side-effect of normalizing input struct
    return earlier_date;
}

// =====================================================================================================================
// Signal handling

// Whether we've received a SIGINT/SIGTERM.
static volatile sig_atomic_t shutdown_requested = 0;

// SIGINT/SIGTERM handler: set shutdown_requested to 1.
static void handle_signal(int sig) {
    (void) sig; // Silence unused parameter warning.
    shutdown_requested = 1;
}

// =====================================================================================================================
// Main function!

int main(void) {
    // Install SIGINT/SIGTERM signal handlers.
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Seed the RNG. Weak, but only used for random sleep jitter.
    srand(ytcd_time());

    char* bgutil_ytdlp_pot_provider_url = ytcd_getenv("YTCD_BGUTIL_YTDLP_POT_PROVIDER_URL");

    if (strlen(bgutil_ytdlp_pot_provider_url) > MAX_POT_PROVIDER_URL_LEN) {
        fprintf(stderr, "[ytcd] FATAL: URL is too long: %s\n", bgutil_ytdlp_pot_provider_url);
        exit(1);
    }
    snprintf(
        bgutil_ytdlp_pot_provider_extractor_args,
        sizeof bgutil_ytdlp_pot_provider_extractor_args,
        "%s%s",
        BGUTIL_YTDLP_POT_PROVIDER_EXTRACTOR_ARGS_PREFIX,
        bgutil_ytdlp_pot_provider_url
    );

    // Wait for the pot provider server to seem healthy. We implement this as a one-time ping on startup rather than
    // leverage a Docker health check, because we trust the pot provider to stay alive and healthy, whereas Docker
    // health checks are continuous. This is just so we don't try to hit the pot provider before it's up, which Docker's
    // "depends_on" doesn't do very well.
    ytcd_curl_global_init(CURL_GLOBAL_NOTHING);
    {
        CURL* curl = ytcd_curl_easy_init();
        snprintf(
            bgutil_ytdlp_pot_provider_ping_url,
            sizeof bgutil_ytdlp_pot_provider_ping_url,
            "%s/ping",
            bgutil_ytdlp_pot_provider_url
        );
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // doubt we'll get a 3xx but hey
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
        curl_easy_setopt(curl, CURLOPT_URL, bgutil_ytdlp_pot_provider_ping_url);
        while (true) {
            if (curl_easy_perform(curl) == CURLE_OK) {
                curl_easy_cleanup(curl);
                curl_global_cleanup();
                break;
            }
            else {
                fprintf(stderr, "[ytcd] INFO: Waiting for pot provider server\n");
                sleep(1);
            }
        }
    }

    // Loop forever: read the channels file, download new videos for each of the channels in it.
    while (shutdown_requested == 0) {
        // Get the current day for this run.
        struct tm today;
        {
            time_t now = ytcd_time();
            ytcd_localtime(&now, &today);
        }

        fprintf(
            stderr,
            "[ytcd] INFO: Running on %04d-%02d-%02d\n",
            today.tm_year + 1900,
            today.tm_mon + 1,
            today.tm_mday
        );

        // Construct a yt-dlp couple arguments that depend on the current day.
        {
            struct tm two_days_ago = subtract_days(today, 2);
            strftime(
                break_match_filters_arg,
                sizeof break_match_filters_arg,
                "upload_date >= %Y%m%d",
                &two_days_ago
            );
            strftime(datebefore_arg, sizeof datebefore_arg, "%Y%m%d", &two_days_ago);
        }

        // Open the channels file.
        FILE* channels_file = ytcd_fopen("/etc/ytcd/channels.txt", "r");

        // Process each channel.
        while (fgets(channel_name, sizeof channel_name, channels_file)) {
            if (shutdown_requested) {
                break;
            }

            size_t channel_name_len = strlen(channel_name);

            // We might read a channel name that's 1 character too long, but that's sometimes ok, because fgets() reads
            // the trailing newline, too.
            if (channel_name_len == MAX_YOUTUBE_CHANNEL_NAME_LEN + 1 && channel_name[MAX_YOUTUBE_CHANNEL_NAME_LEN] != '\n') {
                // Cover up the last character, so the elipses in the error message don't imply there's more when there
                // isn't, for a line that's exactly 1 character too long.
                channel_name[MAX_YOUTUBE_CHANNEL_NAME_LEN] = '\0';
                fprintf(stderr, "[ytcd] ERROR: Channel name is invalid: %s...\n", channel_name);
                continue;
            }

            // Strip trailing newline, if any.
            if (channel_name[channel_name_len - 1] == '\n') {
                channel_name[channel_name_len - 1] = '\0';
                channel_name_len--;
            }

            // Allow empty lines, but skip them.
            if (channel_name_len == 0) {
                continue;
            }

            // Assert that the YouTube channel name contains only valid characters.
            if (is_invalid_youtube_channel_name(channel_name, channel_name_len)) {
                fprintf(stderr, "[ytcd] ERROR: Channel name is invalid: %s\n", channel_name);
                continue;
            }

            // We're ready to call yt-dlp!

            // Fork a child process.
            pid_t pid = ytcd_fork();

            // In the child process, exec yt-dlp.
            if (pid == 0) {
                snprintf(
                    channel_videos_url,
                    sizeof channel_videos_url,
                    "https://www.youtube.com/@%s/videos",
                    channel_name
                );

                char* const yt_dlp_args[] = {
                    "yt-dlp",
                    // When walking backwards in time from the latest video, eventually we'll hit something that's too
                    // old. When we do, stop processing (because we know everything after is even older).
                    "--break-match-filters", break_match_filters_arg,
                    // webp is the default, but we like jpg, because TVs understand them better.
                    "--convert-thumbnails", "jpg",
                    // When walking backwards in time from the latest videos, we want to skip over just-uploaded stuff,
                    // to give the SponsorBlock database a bit of time to fill out.
                    "--datebefore", datebefore_arg,
                    // Remember every video we've downloaded in a constantly-growing archive file. This isn't normally
                    // necessary, as we only wake up once a day to download videos that are exactly two days old.
                    // However, it does allow us to start & stop the process throughout the day and not worry about
                    // re-downloading things.
                    "--download-archive", "/var/lib/ytcd/data/archive.txt",
                    "--embed-metadata",
                    // Embed subtitles in the .mp4.
                    "--embed-subs",
                    "--extractor-args", "youtube:fetch_pot=always;player_client=mweb",
                    "--extractor-args", bgutil_ytdlp_pot_provider_extractor_args,
                    "--extractor-args", "youtubetab:approximate_date",
                    "--file-access-retries", "0",
                    // Prefer mp4 for its wide support in TVs
                    "--format-sort", "vcodec:h264,lang,quality,res,fps,hdr:12,acodec:aac",
                    // Don't download all video metadata upfront, since we are only downloading, at most, a tiny number
                    // of videos that were relatively recently uploaded.
                    "--lazy-playlist",
                    // Sleep for up to 30 seconds between downloads.
                    "--max-sleep-interval", "30",
                    "--merge-output-format", "mp4",
                    "--no-progress",
                    // Put every video of a channel in the same folder. Prefix by upload date, so manually deleting old
                    // stuff from the filesystem is more straightforward. Omit video id even though it uniquely
                    // identifies a video, for cleaner filenames. This prevents us from saving two videos uploaded by
                    // the same channel on the same day with the same title, but that's ok with me.
                    "--output", "/var/lib/ytcd/videos/%(channel)s/Season 01/%(upload_date>%Y-%m-%d)s %(title)s.%(ext)s",
                    "--output", "thumbnail:/var/lib/ytcd/videos/%(channel)s/Season 01/%(upload_date>%Y-%m-%d)s %(title)s-thumb.%(ext)s",
                    "--output", "pl_thumbnail:/var/lib/ytcd/videos/%(channel)s/cover.jpg",
                    "--remux-video", "mp4",
                    // Sleep for at least 20 seconds between downloads.
                    "--sleep-interval", "20",
                    // Sleep for 0.75 seconds between grabbing little fragments of metadata.
                    "--sleep-requests", "0.75",
                    "--sleep-subtitles", "5",
                    // Remove self promo and sponsored segments. There are more, see SponsorBlock.
                    "--sponsorblock-remove", "selfpromo,sponsor",
                    // Uncomment this and rebuild to get a lot more output from yt-dlp
                    // "--verbose",
                    // Save video thumbnails.
                    "--write-thumbnail",
                    channel_videos_url,
                    NULL
                };

                fprintf(stderr, "[ytcd] INFO: Executing yt-dlp for channel %s\n", channel_name);
                ytcd_execvp(yt_dlp_args[0], yt_dlp_args);
            }

            // Wait for yt-dlp to exit
            int status = 0;
            while (true) {
                if (waitpid(pid, &status, 0) < 0) {
                    if (errno == EINTR) {
                        // If we got a SIGINT or SIGTERM, send it along to the child, since we're running with Docker's
                        // built-in 'init' process, which only signals us, not the entire process group.
                        if (shutdown_requested) {
                            kill(pid, SIGTERM);
                        }
                        continue;
                    }
                    perror("[ytcd] FATAL: waitpid");
                    exit(1);
                }
                break;
            }

            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                fprintf(stderr, "[ytcd] INFO: yt-dlp exited with code %d\n", code);
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                fprintf(stderr, "[ytcd] INFO: yt-dlp killed by signal %d\n", sig);
                exit(128 + sig);
            }
        }

        fclose(channels_file);

        if (shutdown_requested == 1) {
            break;
        }

        // Sleep until a random time between 12:30am and 4:00am. Since we don't need `today` anymore, we can just reuse
        // the struct.
        today.tm_mday += 1;
        today.tm_hour = 0;
        today.tm_min = 30 + (rand() % 211); // 30-240 minutes past midnight = 12:30-4:00am
        today.tm_sec = 0;
        today.tm_isdst = -1;
        double seconds_to_sleep = difftime(ytcd_mktime(&today), ytcd_time());
        if (seconds_to_sleep > 0) {
            fprintf(
                stderr,
                "[ytcd] INFO: Sleeping until %02d:%02dam on %04d-%02d-%02d\n",
                today.tm_hour == 0 ? 12 : today.tm_hour,
                today.tm_min,
                today.tm_year + 1900,
                today.tm_mon + 1,
                today.tm_mday
            );
            sleep((unsigned int) seconds_to_sleep);
        }
    }

    return 0;
}
