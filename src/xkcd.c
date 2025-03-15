#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#include "json.h"

// TODO: update usage
void usage(const char *program_name) {
    printf("Usage: %s <number>\n", program_name);
}

#define READ_END  0
#define WRITE_END 1
bool cmd_run_get_output_and_reset(Cmd *cmd, String_Builder *out) {
    bool result = true;

    Proc proc = INVALID_PROC;
    Fd pipe_fds[2] = {INVALID_FD, INVALID_FD};
    if (pipe(pipe_fds) < 0) {
        nob_log(ERROR, "Could not create pipe: %s", strerror(errno));
        return_defer(false);
    }
    proc = cmd_run_async_redirect_and_reset(cmd, (Cmd_Redirect) {
        .fdout = &pipe_fds[WRITE_END],
    });
    if (proc == INVALID_PROC) return_defer(false);

    for (;;) {
        if (out->count >= out->capacity) {
            out->capacity = out->capacity == 0 ? NOB_DA_INIT_CAP : out->capacity*2;
            out->items = realloc(out->items, out->capacity*sizeof(*out->items));
            assert(out->items != NULL && "Buy more RAM lol");
        }
        char *buf = out->items + out->count;
        size_t buf_len = out->capacity - out->count;
        assert(buf_len > 0);

        int n = read(pipe_fds[READ_END], buf, buf_len);
        if (n < 0) {
            nob_log(ERROR, "Could not read from pipe: %s", strerror(errno));
            return_defer(false);
        }
        if (n == 0) break; // All output read
        out->count += n;
    }

defer:
    if (!proc_wait(proc)) return false;
    fd_close(pipe_fds[0]);
    fd_close(pipe_fds[1]);
    return result;
}

String_View jstring_to_sv(json_string_t jstring) {
    return (String_View) { .data = jstring.string, .count = jstring.string_size };
}

json_object_element_t *find_key(json_object_t *object, String_View key) {
    for (json_object_element_t *element = object->start; element != NULL; element = element->next) {
        String_View elt_key = jstring_to_sv(*element->name);
        if (sv_eq(key, elt_key)) return element;
    }
    return NULL;
}

String_View find_string(json_object_t *object, String_View key) {
    json_object_element_t *elt = find_key(object, key);
    if (elt == NULL) {
        nob_log(ERROR, "Couldn't find key \""SV_Fmt"\" in JSON", SV_Arg(key));
        return (String_View) {0};
    }
    json_string_t *jstring = json_value_as_string(elt->value);
    if (jstring == NULL) {
        nob_log(ERROR, "\""SV_Fmt"\" field of JSON was not a string", SV_Arg(key));
        return (String_View) {0};
    }
    return jstring_to_sv(*jstring);
}

typedef struct {
    bool open_in_image_viewer;
    bool print_transcript;
    const char *number;
} Options;
const Options default_options = {
    .open_in_image_viewer = true,
    .print_transcript = true,
    .number = NULL,
};

typedef struct {
    char short_flag_name;
    const char *long_flag_name;
    size_t offset;
} Boolean_Option;

Boolean_Option boolean_options[] = {
    { .offset = offsetof(Options, open_in_image_viewer), .short_flag_name = 'o', .long_flag_name = "open" },
    { .offset = offsetof(Options, print_transcript), .short_flag_name = 'p', .long_flag_name = "print" },
};

int main(int argc, char **argv) {
    const char *program_name = shift(argv, argc);

    Options options = default_options;
    while (argc > 0) {
        const char *arg = shift(argv, argc);
        if (*arg != '-') {
            options.number = arg;
            continue;
        }
        arg++;
        if (*arg == '-') {
            arg++;
            bool value = true;
            if (sv_starts_with(sv_from_cstr(arg), sv_from_cstr("no-"))) {
                value = false;
                arg += 3;
            }
            bool found = false;
            for (size_t i = 0; i < ARRAY_LEN(boolean_options); i++) {
                Boolean_Option opt = boolean_options[i];
                if (strcmp(arg, opt.long_flag_name) == 0) {
                    bool *flag_loc = (bool*)((uint8_t*)&options + opt.offset);
                    *flag_loc = value;
                    found = true;
                }
            }
            if (!found) {
                usage(program_name);
                nob_log(ERROR, "Unknown flag '%s'", arg);
                return 1;
            }
        } else {
            while (*arg) {
                char flag = tolower((unsigned char) *arg);
                bool found = false;
                for (size_t i = 0; i < ARRAY_LEN(boolean_options); i++) {
                    Boolean_Option opt = boolean_options[i];
                    if (flag == tolower(opt.short_flag_name)) {
                        bool *flag_loc = (bool*)((uint8_t*)&options + opt.offset);
                        *flag_loc = islower(*arg);
                        found = true;
                    }
                }
                if (!found) {
                    usage(program_name);
                    nob_log(ERROR, "Unknown flag '%c'", flag);
                    return 1;
                }
                arg++;
            }
        }
    }

    if (options.number == NULL) {
        usage(program_name);
        nob_log(ERROR, "no comic number provided");
        return 1;
    }

    Cmd cmd = {0};
    cmd_append(&cmd, "curl", temp_sprintf("https://xkcd.com/%s/info.0.json", options.number));

    String_Builder response_sb = {0};
    if (!cmd_run_get_output_and_reset(&cmd, &response_sb)) return 1;
    String_View response = sb_to_sv(response_sb);

    json_value_t *root_value = json_parse(response.data, response.count);
    json_object_t *root = root_value ? json_value_as_object(root_value) : NULL;
    if (root == NULL) {
        nob_log(ERROR, "Could not parse JSON returned by XKCD server");
        nob_log(INFO, "Here is the response of the server:");
        printf(SV_Fmt"\n", SV_Arg(response));
        return 1;
    }

    if (options.open_in_image_viewer) {
        String_View img_link = find_string(root, sv_from_cstr("img"));
        if (img_link.data == NULL) return 1;
        cmd_append(&cmd, "feh", "-Z", temp_sv_to_cstr(img_link));
        if (!cmd_run_sync_and_reset(&cmd)) return 1;
    }

    if (options.print_transcript) {
        String_View transcript = find_string(root, sv_from_cstr("transcript"));
        if (transcript.data != NULL && transcript.count != 0) printf(SV_Fmt"\n", SV_Arg(transcript));
    }

    return 0;
}
