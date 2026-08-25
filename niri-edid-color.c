#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/vt.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define TOOL_VERSION "0.1.0"
#define CONNECTOR_NAME "eDP-1"
#define EXPECTED_EDID_LEN 128U
#define MAX_LUT_ENTRIES 1048576U
#define EXPECTED_EDID_SHA256 \
    "cda4deb364442ee30054c0fb9455d85c50e10c50645eb1021704d1d4e96c55d1"

/*
 * Complete EDID for the target Chimei Innolux N140HCA-EAC panel.
 * Matching all 128 bytes keeps this experimental utility from touching a
 * different display merely because it happens to be connected as eDP-1.
 */
static const uint8_t expected_edid[EXPECTED_EDID_LEN] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x0d, 0xae, 0xd4, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x24, 0x1a, 0x01, 0x04, 0xa5, 0x1f, 0x11, 0x78, 0x02, 0x28, 0x65, 0x97, 0x59, 0x54, 0x8e, 0x27,
    0x1e, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xb4, 0x3b, 0x80, 0x4a, 0x71, 0x38, 0x34, 0x40, 0x50, 0x3c,
    0x68, 0x00, 0x35, 0xad, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x4e, 0x31, 0x34,
    0x30, 0x48, 0x43, 0x41, 0x2d, 0x45, 0x41, 0x43, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x43,
    0x4d, 0x4e, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x4e, 0x31, 0x34, 0x30, 0x48, 0x43, 0x41, 0x2d, 0x45, 0x41, 0x43, 0x0a, 0x20, 0x00, 0x05,
};

/* Linear-light sRGB/D65 to the panel primaries and EDID white point. */
static const double panel_matrix[3][3] = {
    { 1.135772694, -0.253609549,  0.109333397 },
    {-0.077937457,  1.261426267, -0.181034903 },
    {-0.017785377, -0.035722018,  1.056546260 },
};

enum command {
    COMMAND_NONE,
    COMMAND_STATUS,
    COMMAND_PROFILE,
    COMMAND_SELF_TEST,
    COMMAND_APPLY,
    COMMAND_RESET,
};

struct options {
    enum command command;
    bool dry_run;
    bool takeover;
    bool force;
    bool allow_bypass_gamma;
};

struct target_device {
    char sysfs_path[PATH_MAX];
    char card_path[64];
};

struct color_properties {
    uint32_t degamma_lut;
    uint32_t degamma_lut_size;
    uint32_t ctm;
    uint32_t gamma_lut;
    uint32_t gamma_lut_size;
    uint64_t degamma_value;
    uint64_t degamma_size;
    uint64_t ctm_value;
    uint64_t gamma_value;
    uint64_t gamma_size;
};

enum blob_state {
    BLOB_BYPASS,
    BLOB_OURS,
    BLOB_OTHER,
    BLOB_UNREADABLE,
};

struct vt_guard {
    int fd;
    int temporary_fd;
    int original_vt;
    int temporary_vt;
    bool switched;
    bool signals_blocked;
    sigset_t old_mask;
};

static struct vt_guard global_vt = {
    .fd = -1,
    .temporary_fd = -1,
};

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: niri-edid-color COMMAND [OPTIONS]\n"
            "\n"
            "Commands:\n"
            "  status       Inspect the target panel and KMS colour properties (read-only)\n"
            "  profile      Print the embedded EDID identity and colour transform\n"
            "  self-test    Test the embedded EDID, matrix, and LUT generation offline\n"
            "  apply        Apply sRGB degamma + the EDID-derived CTM\n"
            "  reset        Put DEGAMMA_LUT and CTM back into hardware bypass\n"
            "\n"
            "Options for apply/reset:\n"
            "  --dry-run                Perform only DRM's atomic TEST_ONLY commit\n"
            "  --takeover               Temporarily switch to a free VT to obtain DRM master\n"
            "  --force                  Overwrite colour state not created by this tool\n"
            "  --allow-bypass-gamma     Apply without an active final gamma curve (will look dark)\n"
            "\n"
            "Safety defaults:\n"
            "  * The complete 128-byte N140HCA-EAC EDID must match.\n"
            "  * Unknown existing DEGAMMA_LUT/CTM state is not overwritten.\n"
            "  * A real apply is refused while GAMMA_LUT is bypassed unless explicitly allowed.\n"
            "  * Every change is validated with an atomic TEST_ONLY commit first.\n");
}

static int parse_options(int argc, char **argv, struct options *options)
{
    memset(options, 0, sizeof(*options));

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        } else if (strcmp(arg, "--version") == 0) {
            puts("niri-edid-color " TOOL_VERSION);
            exit(EXIT_SUCCESS);
        } else if (strcmp(arg, "--dry-run") == 0) {
            options->dry_run = true;
        } else if (strcmp(arg, "--takeover") == 0) {
            options->takeover = true;
        } else if (strcmp(arg, "--force") == 0) {
            options->force = true;
        } else if (strcmp(arg, "--allow-bypass-gamma") == 0) {
            options->allow_bypass_gamma = true;
        } else if (strcmp(arg, "status") == 0 || strcmp(arg, "profile") == 0 ||
                   strcmp(arg, "self-test") == 0 || strcmp(arg, "apply") == 0 ||
                   strcmp(arg, "reset") == 0) {
            if (options->command != COMMAND_NONE) {
                fputs("multiple commands were provided\n", stderr);
                return -1;
            }
            if (strcmp(arg, "status") == 0) {
                options->command = COMMAND_STATUS;
            } else if (strcmp(arg, "profile") == 0) {
                options->command = COMMAND_PROFILE;
            } else if (strcmp(arg, "self-test") == 0) {
                options->command = COMMAND_SELF_TEST;
            } else if (strcmp(arg, "apply") == 0) {
                options->command = COMMAND_APPLY;
            } else {
                options->command = COMMAND_RESET;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg);
            return -1;
        }
    }

    if (options->command == COMMAND_NONE) {
        fputs("a command is required\n", stderr);
        return -1;
    }

    if (options->command != COMMAND_APPLY && options->command != COMMAND_RESET &&
        (options->dry_run || options->takeover || options->force ||
         options->allow_bypass_gamma)) {
        fputs("apply/reset options were provided for a read-only command\n", stderr);
        return -1;
    }

    if (options->command == COMMAND_RESET && options->allow_bypass_gamma) {
        fputs("--allow-bypass-gamma is only meaningful with apply\n", stderr);
        return -1;
    }

    return 0;
}

static int read_file(const char *path, uint8_t *buffer, size_t capacity, size_t *length)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    size_t used = 0;
    while (used < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - used);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (count == 0) {
            break;
        }
        used += (size_t)count;
    }

    close(fd);
    *length = used;
    return 0;
}

static int discover_target(struct target_device *target)
{
    glob_t matches;
    memset(&matches, 0, sizeof(matches));

    int result = glob("/sys/class/drm/card*-" CONNECTOR_NAME, 0, NULL, &matches);
    if (result != 0) {
        fprintf(stderr, "could not find /sys/class/drm/card*-%s\n", CONNECTOR_NAME);
        globfree(&matches);
        return -1;
    }

    int found = 0;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        char edid_path[PATH_MAX];
        int written = snprintf(edid_path, sizeof(edid_path), "%s/edid", matches.gl_pathv[i]);
        if (written < 0 || (size_t)written >= sizeof(edid_path)) {
            continue;
        }

        uint8_t edid[EXPECTED_EDID_LEN + 1U];
        size_t length = 0;
        if (read_file(edid_path, edid, sizeof(edid), &length) != 0) {
            continue;
        }
        if (length != EXPECTED_EDID_LEN ||
            memcmp(edid, expected_edid, EXPECTED_EDID_LEN) != 0) {
            fprintf(stderr, "ignoring non-matching panel at %s\n", matches.gl_pathv[i]);
            continue;
        }

        const char *base = strrchr(matches.gl_pathv[i], '/');
        base = base == NULL ? matches.gl_pathv[i] : base + 1;

        if (strncmp(base, "card", 4U) != 0) {
            continue;
        }
        errno = 0;
        char *end = NULL;
        long card_number = strtol(base + 4, &end, 10);
        if (errno != 0 || end == base + 4 || card_number < 0 || card_number > INT_MAX ||
            strcmp(end, "-" CONNECTOR_NAME) != 0) {
            continue;
        }

        int sysfs_written = snprintf(target->sysfs_path, sizeof(target->sysfs_path),
                                     "%s", matches.gl_pathv[i]);
        int card_written = snprintf(target->card_path, sizeof(target->card_path),
                                    "/dev/dri/card%ld", card_number);
        if (sysfs_written < 0 || (size_t)sysfs_written >= sizeof(target->sysfs_path) ||
            card_written < 0 || (size_t)card_written >= sizeof(target->card_path)) {
            continue;
        }

        found++;
    }

    globfree(&matches);

    if (found != 1) {
        fprintf(stderr,
                "expected exactly one %s with EDID SHA-256 %s; found %d\n",
                CONNECTOR_NAME, EXPECTED_EDID_SHA256, found);
        return -1;
    }

    return 0;
}

static uint64_t ctm_coefficient(double value)
{
    double magnitude = fabs(value) * 4294967296.0;
    uint64_t encoded = (uint64_t)llround(magnitude);
    encoded &= ~(UINT64_C(1) << 63);
    if (value < 0.0) {
        encoded |= UINT64_C(1) << 63;
    }
    return encoded;
}

static void build_ctm(struct drm_color_ctm *ctm)
{
    for (size_t row = 0; row < 3U; row++) {
        for (size_t column = 0; column < 3U; column++) {
            ctm->matrix[row * 3U + column] = ctm_coefficient(panel_matrix[row][column]);
        }
    }
}

static double srgb_to_linear(double value)
{
    if (value <= 0.04045) {
        return value / 12.92;
    }
    return pow((value + 0.055) / 1.055, 2.4);
}

static struct drm_color_lut *build_degamma(uint32_t size)
{
    if (size < 2U || size > MAX_LUT_ENTRIES) {
        errno = EINVAL;
        return NULL;
    }

    struct drm_color_lut *lut = calloc((size_t)size, sizeof(*lut));
    if (lut == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < size; i++) {
        double input = (double)i / (double)(size - 1U);
        double output = srgb_to_linear(input);
        uint16_t encoded = (uint16_t)llround(output * 65535.0);
        lut[i].red = encoded;
        lut[i].green = encoded;
        lut[i].blue = encoded;
        lut[i].reserved = 0;
    }

    return lut;
}

static void print_profile(void)
{
    puts("Target panel: Chimei Innolux N140HCA-EAC (CMN 0x14D4)");
    puts("Connector:    " CONNECTOR_NAME);
    puts("EDID SHA-256: " EXPECTED_EDID_SHA256);
    puts("EDID gamma:   2.20");
    puts("Pipeline:     sRGB EOTF -> linear-light matrix -> gamma 2.2 regamma client");
    puts("Matrix (linear sRGB/D65 -> panel RGB/EDID white):");
    for (size_t row = 0; row < 3U; row++) {
        printf("  % .9f  % .9f  % .9f\n",
               panel_matrix[row][0], panel_matrix[row][1], panel_matrix[row][2]);
    }
}

static int self_test(void)
{
    unsigned int checksum = 0;
    for (size_t i = 0; i < EXPECTED_EDID_LEN; i++) {
        checksum += expected_edid[i];
    }
    if ((checksum & 0xffU) != 0U) {
        fputs("self-test: embedded EDID checksum is invalid\n", stderr);
        return -1;
    }

    struct drm_color_lut *lut = build_degamma(33U);
    if (lut == NULL) {
        perror("self-test: build_degamma");
        return -1;
    }
    if (lut[0].red != 0U || lut[32].red != UINT16_MAX) {
        fputs("self-test: degamma endpoints are invalid\n", stderr);
        free(lut);
        return -1;
    }
    for (size_t i = 1; i < 33U; i++) {
        if (lut[i].red < lut[i - 1U].red || lut[i].red != lut[i].green ||
            lut[i].red != lut[i].blue) {
            fputs("self-test: degamma LUT is not monotonic and neutral\n", stderr);
            free(lut);
            return -1;
        }
    }
    free(lut);

    struct drm_color_ctm ctm;
    build_ctm(&ctm);
    if ((ctm.matrix[1] & (UINT64_C(1) << 63)) == 0U ||
        (ctm.matrix[0] & (UINT64_C(1) << 63)) != 0U) {
        fputs("self-test: CTM sign-magnitude encoding is invalid\n", stderr);
        return -1;
    }

    for (size_t row = 0; row < 3U; row++) {
        double white = panel_matrix[row][0] + panel_matrix[row][1] + panel_matrix[row][2];
        if (fabs(white - 1.0) > 0.02) {
            fputs("self-test: matrix maps white outside expected tolerance\n", stderr);
            return -1;
        }
    }

    puts("self-test: EDID, degamma LUT, and CTM checks passed");
    return 0;
}

static int find_crtc_for_connector(int fd, uint32_t *connector_id, uint32_t *crtc_id)
{
    drmModeResPtr resources = drmModeGetResources(fd);
    if (resources == NULL) {
        perror("drmModeGetResources");
        return -1;
    }

    int result = -1;
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector == NULL) {
            continue;
        }

        const char *type_name = drmModeGetConnectorTypeName(connector->connector_type);
        char name[64];
        int written = snprintf(name, sizeof(name), "%s-%u",
                               type_name == NULL ? "Unknown" : type_name,
                               connector->connector_type_id);
        bool name_matches = written > 0 && (size_t)written < sizeof(name) &&
                            strcmp(name, CONNECTOR_NAME) == 0;
        if (!name_matches || connector->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(connector);
            continue;
        }

        drmModeEncoderPtr encoder = NULL;
        if (connector->encoder_id != 0U) {
            encoder = drmModeGetEncoder(fd, connector->encoder_id);
        }
        if (encoder == NULL || encoder->crtc_id == 0U) {
            if (encoder != NULL) {
                drmModeFreeEncoder(encoder);
                encoder = NULL;
            }
            for (int j = 0; j < connector->count_encoders; j++) {
                encoder = drmModeGetEncoder(fd, connector->encoders[j]);
                if (encoder != NULL && encoder->crtc_id != 0U) {
                    break;
                }
                if (encoder != NULL) {
                    drmModeFreeEncoder(encoder);
                    encoder = NULL;
                }
            }
        }

        if (encoder == NULL || encoder->crtc_id == 0U) {
            fprintf(stderr, "%s is connected but has no active CRTC\n", CONNECTOR_NAME);
            if (encoder != NULL) {
                drmModeFreeEncoder(encoder);
            }
            drmModeFreeConnector(connector);
            break;
        }

        *connector_id = connector->connector_id;
        *crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        result = 0;
        break;
    }

    drmModeFreeResources(resources);
    if (result != 0) {
        fprintf(stderr, "could not find connected, active %s\n", CONNECTOR_NAME);
    }
    return result;
}

static int load_color_properties(int fd, uint32_t crtc_id, struct color_properties *properties)
{
    memset(properties, 0, sizeof(*properties));

    drmModeObjectPropertiesPtr object =
        drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    if (object == NULL) {
        perror("drmModeObjectGetProperties(CRTC)");
        return -1;
    }

    for (uint32_t i = 0; i < object->count_props; i++) {
        drmModePropertyPtr property = drmModeGetProperty(fd, object->props[i]);
        if (property == NULL) {
            continue;
        }

        uint32_t id = property->prop_id;
        uint64_t value = object->prop_values[i];
        if (strcmp(property->name, "DEGAMMA_LUT") == 0) {
            properties->degamma_lut = id;
            properties->degamma_value = value;
        } else if (strcmp(property->name, "DEGAMMA_LUT_SIZE") == 0) {
            properties->degamma_lut_size = id;
            properties->degamma_size = value;
        } else if (strcmp(property->name, "CTM") == 0) {
            properties->ctm = id;
            properties->ctm_value = value;
        } else if (strcmp(property->name, "GAMMA_LUT") == 0) {
            properties->gamma_lut = id;
            properties->gamma_value = value;
        } else if (strcmp(property->name, "GAMMA_LUT_SIZE") == 0) {
            properties->gamma_lut_size = id;
            properties->gamma_size = value;
        }

        drmModeFreeProperty(property);
    }

    drmModeFreeObjectProperties(object);

    if (properties->degamma_lut == 0U || properties->degamma_lut_size == 0U ||
        properties->ctm == 0U) {
        fputs("required CRTC properties DEGAMMA_LUT, DEGAMMA_LUT_SIZE, and CTM are missing\n",
              stderr);
        return -1;
    }
    if (properties->degamma_size < 2U || properties->degamma_size > MAX_LUT_ENTRIES) {
        fprintf(stderr, "unsupported DEGAMMA_LUT_SIZE: %llu\n",
                (unsigned long long)properties->degamma_size);
        return -1;
    }

    return 0;
}

static enum blob_state classify_blob(int fd, uint64_t blob_id,
                                     const void *expected, size_t expected_length)
{
    if (blob_id == 0U) {
        return BLOB_BYPASS;
    }

    drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
    if (blob == NULL) {
        return BLOB_UNREADABLE;
    }

    enum blob_state state = BLOB_OTHER;
    if (blob->length == expected_length && memcmp(blob->data, expected, expected_length) == 0) {
        state = BLOB_OURS;
    }
    drmModeFreePropertyBlob(blob);
    return state;
}

static const char *blob_state_name(enum blob_state state)
{
    switch (state) {
    case BLOB_BYPASS:
        return "bypass";
    case BLOB_OURS:
        return "our EDID transform";
    case BLOB_OTHER:
        return "other/unknown";
    case BLOB_UNREADABLE:
        return "unreadable blob";
    }
    return "invalid";
}

static int build_expected_blobs(const struct color_properties *properties,
                                struct drm_color_lut **degamma,
                                size_t *degamma_length,
                                struct drm_color_ctm *ctm)
{
    uint32_t size = (uint32_t)properties->degamma_size;
    *degamma = build_degamma(size);
    if (*degamma == NULL) {
        perror("build_degamma");
        return -1;
    }
    *degamma_length = (size_t)size * sizeof(**degamma);
    build_ctm(ctm);
    return 0;
}

static int print_status(int fd, const struct target_device *target,
                        uint32_t connector_id, uint32_t crtc_id,
                        const struct color_properties *properties)
{
    struct drm_color_lut *degamma = NULL;
    size_t degamma_length = 0;
    struct drm_color_ctm ctm;
    if (build_expected_blobs(properties, &degamma, &degamma_length, &ctm) != 0) {
        return -1;
    }

    enum blob_state degamma_state = classify_blob(fd, properties->degamma_value,
                                                   degamma, degamma_length);
    enum blob_state ctm_state = classify_blob(fd, properties->ctm_value,
                                              &ctm, sizeof(ctm));
    free(degamma);

    drmVersionPtr version = drmGetVersion(fd);
    printf("target:          %s (%s)\n", CONNECTOR_NAME, target->sysfs_path);
    printf("card:            %s", target->card_path);
    if (version != NULL && version->name != NULL) {
        printf(" (%s)", version->name);
    }
    putchar('\n');
    printf("connector/CRTC:  %u / %u\n", connector_id, crtc_id);
    printf("DRM master:      %s\n", drmIsMaster(fd) == 1 ? "yes" : "no (normal while niri runs)");
    printf("DEGAMMA_LUT:     %s (blob %llu, size %llu)\n",
           blob_state_name(degamma_state),
           (unsigned long long)properties->degamma_value,
           (unsigned long long)properties->degamma_size);
    printf("CTM:             %s (blob %llu)\n",
           blob_state_name(ctm_state),
           (unsigned long long)properties->ctm_value);
    if (properties->gamma_lut != 0U) {
        printf("GAMMA_LUT:       %s (blob %llu, size %llu)\n",
               properties->gamma_value == 0U ? "bypass" : "active (owned externally)",
               (unsigned long long)properties->gamma_value,
               (unsigned long long)properties->gamma_size);
    } else {
        puts("GAMMA_LUT:       unavailable");
    }
    printf("EDID SHA-256:    %s (exact embedded EDID matched)\n", EXPECTED_EDID_SHA256);

    if (version != NULL) {
        drmFreeVersion(version);
    }
    return 0;
}

static int ensure_master(int fd)
{
    if (drmIsMaster(fd) == 1) {
        return 0;
    }

    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 50000000L,
    };
    for (int attempt = 0; attempt < 40; attempt++) {
        if (drmSetMaster(fd) == 0 || drmIsMaster(fd) == 1) {
            return 0;
        }
        if (errno != EBUSY && errno != EACCES) {
            break;
        }
        nanosleep(&delay, NULL);
    }

    fprintf(stderr,
            "could not obtain DRM master on the card: %s\n"
            "stop the compositor first or use --takeover as root\n",
            strerror(errno));
    return -1;
}

static void restore_vt(void)
{
    if (global_vt.fd >= 0 && global_vt.switched) {
        if (ioctl(global_vt.fd, VT_ACTIVATE, global_vt.original_vt) != 0) {
            fprintf(stderr, "warning: could not reactivate VT %d: %s\n",
                    global_vt.original_vt, strerror(errno));
        } else if (ioctl(global_vt.fd, VT_WAITACTIVE, global_vt.original_vt) != 0) {
            fprintf(stderr, "warning: VT %d did not report active: %s\n",
                    global_vt.original_vt, strerror(errno));
        }
        global_vt.switched = false;
    }
    if (global_vt.temporary_fd >= 0) {
        close(global_vt.temporary_fd);
        global_vt.temporary_fd = -1;
    }
    if (global_vt.fd >= 0) {
        close(global_vt.fd);
        global_vt.fd = -1;
    }
    if (global_vt.signals_blocked) {
        sigprocmask(SIG_SETMASK, &global_vt.old_mask, NULL);
        global_vt.signals_blocked = false;
    }
}

static int take_over_vt(void)
{
    if (geteuid() != 0) {
        fputs("--takeover requires root (run the built binary with sudo)\n", stderr);
        return -1;
    }

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &block, &global_vt.old_mask) != 0) {
        perror("sigprocmask");
        return -1;
    }
    global_vt.signals_blocked = true;

    global_vt.fd = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (global_vt.fd < 0) {
        perror("open /dev/tty0");
        restore_vt();
        return -1;
    }

    struct vt_stat state;
    memset(&state, 0, sizeof(state));
    if (ioctl(global_vt.fd, VT_GETSTATE, &state) != 0) {
        perror("VT_GETSTATE");
        restore_vt();
        return -1;
    }

    int free_vt = -1;
    if (ioctl(global_vt.fd, VT_OPENQRY, &free_vt) != 0 || free_vt <= 0) {
        perror("VT_OPENQRY");
        restore_vt();
        return -1;
    }

    global_vt.original_vt = (int)state.v_active;
    global_vt.temporary_vt = free_vt;

    char temporary_path[64];
    int written = snprintf(temporary_path, sizeof(temporary_path),
                           "/dev/tty%d", global_vt.temporary_vt);
    if (written < 0 || (size_t)written >= sizeof(temporary_path)) {
        fputs("temporary VT path is too long\n", stderr);
        restore_vt();
        return -1;
    }
    global_vt.temporary_fd = open(temporary_path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (global_vt.temporary_fd < 0) {
        fprintf(stderr, "open %s: %s\n", temporary_path, strerror(errno));
        restore_vt();
        return -1;
    }

    global_vt.switched = true;

    fprintf(stderr, "temporarily switching VT %d -> VT %d\n",
            global_vt.original_vt, global_vt.temporary_vt);
    if (ioctl(global_vt.fd, VT_ACTIVATE, global_vt.temporary_vt) != 0 ||
        ioctl(global_vt.fd, VT_WAITACTIVE, global_vt.temporary_vt) != 0) {
        perror("VT switch");
        restore_vt();
        return -1;
    }

    return 0;
}

static int atomic_commit_color(int fd, uint32_t crtc_id,
                               const struct color_properties *properties,
                               bool reset, bool dry_run,
                               const struct drm_color_lut *degamma,
                               size_t degamma_length,
                               const struct drm_color_ctm *ctm)
{
    uint32_t degamma_blob = 0;
    uint32_t ctm_blob = 0;

    if (!reset) {
        if (drmModeCreatePropertyBlob(fd, degamma, degamma_length, &degamma_blob) != 0) {
            perror("drmModeCreatePropertyBlob(DEGAMMA_LUT)");
            return -1;
        }
        if (drmModeCreatePropertyBlob(fd, ctm, sizeof(*ctm), &ctm_blob) != 0) {
            perror("drmModeCreatePropertyBlob(CTM)");
            drmModeDestroyPropertyBlob(fd, degamma_blob);
            return -1;
        }
    }

    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (request == NULL) {
        fputs("drmModeAtomicAlloc failed\n", stderr);
        if (degamma_blob != 0U) {
            drmModeDestroyPropertyBlob(fd, degamma_blob);
        }
        if (ctm_blob != 0U) {
            drmModeDestroyPropertyBlob(fd, ctm_blob);
        }
        return -1;
    }

    int add_degamma = drmModeAtomicAddProperty(request, crtc_id,
                                               properties->degamma_lut,
                                               reset ? 0U : degamma_blob);
    int add_ctm = drmModeAtomicAddProperty(request, crtc_id,
                                           properties->ctm,
                                           reset ? 0U : ctm_blob);
    if (add_degamma < 0 || add_ctm < 0) {
        fputs("could not add colour properties to atomic request\n", stderr);
        drmModeAtomicFree(request);
        if (degamma_blob != 0U) {
            drmModeDestroyPropertyBlob(fd, degamma_blob);
        }
        if (ctm_blob != 0U) {
            drmModeDestroyPropertyBlob(fd, ctm_blob);
        }
        return -1;
    }

    errno = 0;
    if (drmModeAtomicCommit(fd, request, DRM_MODE_ATOMIC_TEST_ONLY, NULL) != 0) {
        fprintf(stderr, "atomic TEST_ONLY commit rejected: %s\n", strerror(errno));
        drmModeAtomicFree(request);
        if (degamma_blob != 0U) {
            drmModeDestroyPropertyBlob(fd, degamma_blob);
        }
        if (ctm_blob != 0U) {
            drmModeDestroyPropertyBlob(fd, ctm_blob);
        }
        return -1;
    }

    puts("atomic TEST_ONLY commit accepted");
    int result = 0;
    if (dry_run) {
        puts("dry run: display state was not changed");
    } else if (drmModeAtomicCommit(fd, request, 0, NULL) != 0) {
        fprintf(stderr, "atomic commit failed after successful test: %s\n", strerror(errno));
        result = -1;
    }

    drmModeAtomicFree(request);
    if (degamma_blob != 0U) {
        drmModeDestroyPropertyBlob(fd, degamma_blob);
    }
    if (ctm_blob != 0U) {
        drmModeDestroyPropertyBlob(fd, ctm_blob);
    }
    return result;
}

static int change_color_state(int fd, uint32_t crtc_id,
                              const struct options *options,
                              const struct color_properties *properties)
{
    struct drm_color_lut *degamma = NULL;
    size_t degamma_length = 0;
    struct drm_color_ctm ctm;
    if (build_expected_blobs(properties, &degamma, &degamma_length, &ctm) != 0) {
        return -1;
    }

    enum blob_state degamma_state = classify_blob(fd, properties->degamma_value,
                                                   degamma, degamma_length);
    enum blob_state ctm_state = classify_blob(fd, properties->ctm_value,
                                              &ctm, sizeof(ctm));
    bool reset = options->command == COMMAND_RESET;

    printf("current DEGAMMA_LUT: %s\n", blob_state_name(degamma_state));
    printf("current CTM:         %s\n", blob_state_name(ctm_state));

    bool unknown_state = degamma_state == BLOB_OTHER || degamma_state == BLOB_UNREADABLE ||
                         ctm_state == BLOB_OTHER || ctm_state == BLOB_UNREADABLE;
    if (unknown_state && !options->force) {
        fputs("refusing to overwrite colour state not created by this tool; use --force only if intentional\n",
              stderr);
        free(degamma);
        return -1;
    }

    if (!reset && !options->dry_run &&
        (properties->gamma_lut == 0U || properties->gamma_value == 0U) &&
        !options->allow_bypass_gamma) {
        fputs("refusing real apply while GAMMA_LUT is bypassed: sRGB degamma without the final\n"
              "gamma 2.2 curve would make the display very dark. Start the regamma client first,\n"
              "or pass --allow-bypass-gamma only for a controlled pre-session setup.\n",
              stderr);
        free(degamma);
        return -1;
    }

    if (reset && degamma_state == BLOB_BYPASS && ctm_state == BLOB_BYPASS) {
        puts("already reset; no commit needed");
        free(degamma);
        return 0;
    }
    if (!reset && degamma_state == BLOB_OURS && ctm_state == BLOB_OURS &&
        !options->dry_run) {
        puts("EDID transform is already applied; no commit needed");
        free(degamma);
        return 0;
    }

    int result = atomic_commit_color(fd, crtc_id, properties, reset,
                                     options->dry_run, degamma,
                                     degamma_length, &ctm);
    free(degamma);
    if (result == 0 && !options->dry_run) {
        puts(reset ? "reset committed: DEGAMMA_LUT and CTM are in bypass"
                   : "apply committed: sRGB DEGAMMA_LUT and EDID CTM are active");
    }
    return result;
}

int main(int argc, char **argv)
{
    struct options options;
    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (options.command == COMMAND_PROFILE) {
        print_profile();
        return EXIT_SUCCESS;
    }
    if (options.command == COMMAND_SELF_TEST) {
        return self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    struct target_device target;
    memset(&target, 0, sizeof(target));
    if (discover_target(&target) != 0) {
        return EXIT_FAILURE;
    }

    if (options.takeover) {
        atexit(restore_vt);
        if (take_over_vt() != 0) {
            return EXIT_FAILURE;
        }
    }

    int fd = open(target.card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", target.card_path, strerror(errno));
        restore_vt();
        return EXIT_FAILURE;
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1U) != 0) {
        fprintf(stderr, "could not enable DRM atomic capability: %s\n", strerror(errno));
        close(fd);
        restore_vt();
        return EXIT_FAILURE;
    }

    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    if (find_crtc_for_connector(fd, &connector_id, &crtc_id) != 0) {
        close(fd);
        restore_vt();
        return EXIT_FAILURE;
    }

    struct color_properties properties;
    if (load_color_properties(fd, crtc_id, &properties) != 0) {
        close(fd);
        restore_vt();
        return EXIT_FAILURE;
    }

    int result;
    if (options.command == COMMAND_STATUS) {
        result = print_status(fd, &target, connector_id, crtc_id, &properties);
    } else {
        if (ensure_master(fd) != 0) {
            close(fd);
            restore_vt();
            return EXIT_FAILURE;
        }
        result = change_color_state(fd, crtc_id, &options, &properties);
    }

    if (drmIsMaster(fd) == 1) {
        drmDropMaster(fd);
    }
    close(fd);
    restore_vt();
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
