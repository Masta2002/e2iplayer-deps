#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#ifndef _MSC_VER
#if !defined(__APPLE__) && !defined(__MINGW32__) && !defined(__CYGWIN__)
#include <sys/prctl.h>
#endif
#include <sys/types.h>   /* off_t for ftello() in resume_checkpoint() */
#include <unistd.h>
#else
#include <Windows.h>
#define sleep Sleep
#endif

#include <inttypes.h>

#include "curl.h"
#include "hls.h"
#include "msg.h"
#include "misc.h"
#include "aes.h"
#include "mpegts.h"

static uint64_t get_duration_ms(const char *ptr)
{
    uint64_t v1 = 0;
    uint64_t v2 = 0;
    uint32_t n = 0;
    bool hasDot = false;

    while (*ptr == ' ' || *ptr == '\t' ) ++ptr;

    while (*ptr != '\0') {
        if (*ptr >= '0' && *ptr <= '9') {
            uint32_t digit = (uint32_t)((*ptr) - '0');
            if (!hasDot)
                v1 = v1 * 10 + digit;
            else if (n < 3) {
                ++n;
                v2 = v2 * 10 + digit;
            }
            else
                break;
        }
        else if (*ptr == '.' && !hasDot) {
            hasDot = true;
        }
        else
            break;
        ++ptr;
    }

    if (v2 > 0)
    while (n < 3) {
        ++n;
        v2 *= 10;
    }

    return v1 * 1000 + v2;
}

static void set_hls_http_header(void *session)
{
    if (hls_args.user_agent) {
        set_user_agent_http_session(session, hls_args.user_agent);
    }

    if (hls_args.proxy_uri) {
        set_proxy_uri_http_session(session, hls_args.proxy_uri);
    }

    if (hls_args.cookie_file) {
        set_cookie_file_session(session, hls_args.cookie_file, hls_args.cookie_file_mutex);
    }

    for (int i=0; i<HLSDL_MAX_NUM_OF_CUSTOM_HEADERS; ++i) {
        if (hls_args.custom_headers[i]) {
            add_custom_header_http_session(session, hls_args.custom_headers[i]);
        }
        else {
            break;
        }
    }
}

/* Never returns NULL: without a session (out of memory, no curl handle) hlsdl
 * cannot go on, and an assert would be compiled out with NDEBUG - so every
 * caller can use the session right away. */
static void * init_hls_session(void)
{
    void *session = init_http_session();
    if (!session) {
        MSG_ERROR("Could not create an HTTP session.\n");
        MSG_API("{\"error_code\":-1, \"error_msg\":\"session\"}\n");
        exit(1);
    }
    set_hls_http_header(session);
    return session;
}

long get_hls_data_from_url(char *url, char **out, size_t *size, int type, char **new_url)
{
    void *session = init_hls_session();
    long http_code = get_data_from_url_with_session(&session, url, out, size, type, new_url, -1, -1);
    clean_http_session(session);
    return http_code;
}

int is_playlist_FPS(char* source)
{
    return strstr(source, "KEYFORMAT=\"com.apple.streamingkeydelivery\"") ||
           strstr(source, "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") ||
           strstr(source, "com.microsoft.playready");
}

int get_playlist_type(char *source)
{
    if (strncmp("#EXTM3U", source, 7) != 0) {
        MSG_WARNING("Not a valid M3U8 file. Exiting.\n");
        return -1;
    }

    if (strstr(source, "#EXT-X-STREAM-INF")) {
        return MASTER_PLAYLIST;
    }

    if (!hls_args.force_ignoredrm && is_playlist_FPS(source)) {
        MSG_WARNING("HLS stream is DRM protected. Exiting\n");
        return -1;
    }

    return MEDIA_PLAYLIST;
}

/*
 * In response to
 * http://stackoverflow.com/questions/1634359/is-there-a-reverse-fn-for-strstr
 *
 * Basically, strstr but return last occurence, not first.
 *
 * This file contains several implementations and a harness to test and
 * benchmark them.
 *
 * Some of the implementations of the actual function are copied from
 * elsewhere; they are commented with the location. The rest of the coe
 * was written by Lars Wirzenius (liw@liw.fi) and is hereby released into
 * the public domain. No warranty. If it turns out to be broken, you get
 * to keep the pieces.
 */

 /* By liw. */
static char* last_strstr(const char* haystack, const char* needle)
{
    if (*needle == '\0')
        return (char*)haystack;

    char* result = NULL;
    for (;;) {
        char* p = strstr(haystack, needle);
        if (p == NULL)
            break;
        result = p;
        haystack = p + 1;
    }

    return result;
}
static int extend_url(char **url, const char *baseurl)
{
    static const char proxy_marker[] = "englandproxy.co.uk"; // ugly workaround to be fixed
    static const char proxy_url[] = "http://www.englandproxy.co.uk/";
    size_t max_length = strlen(*url) + strlen(baseurl) + 10;

    if (!strncmp(*url, "http://", 7) || !strncmp(*url, "https://", 8)) {
        if (strstr(baseurl, proxy_marker) && !strstr(*url, proxy_marker)) {
            max_length = strlen(*url) + strlen(proxy_url);
            char *buffer = malloc(max_length);
            snprintf(buffer, max_length, "%s%s", proxy_url, strstr(*url, "://") + 3);
            free(*url);
            *url = buffer;
        }
        return 0;
    }
    else if (**url == '/') {
        char *domain = malloc(max_length);
        strcpy(domain, baseurl);
        char proto[6];
        // domain holds max_length > strlen(baseurl) bytes, so %[^/] cannot overflow
        // cppcheck-suppress invalidscanf
        if( 2 == sscanf(baseurl, "%5[^:]://%[^/]", proto, domain))
        {
            char *buffer = malloc(max_length);
            if ( (*url)[1] == '/') // url start with "//"
            {
                snprintf(buffer, max_length, "%s:%s", proto, *url);
            }
            else
            {
                snprintf(buffer, max_length, "%s://%s%s", proto, domain, *url);
            }
            free(*url);
            *url = buffer;
        }
        free(domain);
        return 0;
    }
    else if (strstr(baseurl, "://")) {
        // URLs can have '?'. To make /../ work, remove it.
        char *domain = strdup(baseurl);
        char *find_questionmark = strchr(domain, '?');
        if (find_questionmark) {
            *find_questionmark = '\0';
        }

        char *buffer = malloc(max_length);
        snprintf(buffer, max_length, "%s/../%s", domain, *url);
        free(*url);
        *url = buffer;
        free(domain);
        return 0;
    }
    else {
        // Assume local URL
        char* folder = strdup(baseurl);
        char* separator = "/";
        char* find_folder = last_strstr(folder, separator);
        if (find_folder) {
            *find_folder = '\0';
        } else {
            separator = "\\";
            find_folder = last_strstr(folder, separator);
            if (find_folder)
                *find_folder = '\0';
        }
        if (find_folder) {
            char* buffer = malloc(max_length);
            snprintf(buffer, max_length, "%s%s%s", folder, separator, *url);
            free(*url);
            *url = buffer;
        }
        free(folder);
        return 0;
    }
}

static void segment_list_append(hls_media_segment_t** first, hls_media_segment_t** last, hls_media_segment_t* ms)
{
    if (*first == NULL)
    {
        *first = ms;
        *last = ms;
    }
    else
    {
        (*last)->next = ms;
        ms->prev = *last;
        *last = ms;
    }
}

static char* parse_byterange(char *tag, int64_t *seg_offset, int64_t *seg_size)
{
    *seg_size = strtoll(tag, &tag, 10);
    tag = strchr(tag, '@');
    if (tag) {
        *seg_offset = strtoll(tag+1, &tag, 10);
    }
    return tag;
}

static bool get_next_attrib(char **source, char **tag, char **val);

/* Next NAME=value pair of a tag's attribute list. Unlike get_next_attrib()
 * alone it does not stop at an attribute with an empty value (KEYFORMAT="").
 * A quoted value may hold commas. */
static bool next_tag_attrib(char **source, char **name, char **value)
{
    while (**source) {
        char *before = *source;
        if (get_next_attrib(source, name, value)) {
            return true;
        }
        if (*source == before) {
            break;
        }
    }
    return false;
}

/* EXT-X-KEY: the attributes may come in any order (RFC 8216 4.2), METHOD
 * included. An unknown or missing METHOD leaves the key state as it is; a key
 * without URI keeps the previous key URL. */
static int parse_key(hls_media_playlist_t *me, char *attrs)
{
    char *name = NULL;
    char *value = NULL;
    char *method = NULL;
    char *uri = NULL;
    char *iv = NULL;
    int enc_type;

    while (next_tag_attrib(&attrs, &name, &value)) {
        if (!strcmp(name, "METHOD")) {
            method = value;
        } else if (!strcmp(name, "URI")) {
            uri = value;
        } else if (!strcmp(name, "IV")) {
            iv = value;
        }
    }

    if (!method) {
        return 1;
    } else if (!strcmp(method, "NONE")) {
        /* the segments that follow are in the clear */
        me->current_encryptiontype = ENC_NONE;
        return 0;
    } else if (!strcmp(method, "AES-128")) {
        enc_type = ENC_AES128;
        me->enc_aes.iv_is_static = false;
    } else if (!strcmp(method, "SAMPLE-AES-CTR")) {
        enc_type = ENC_AES_SAMPLE_CTR;
        me->enc_aes.iv_is_static = is_playlist_FPS(me->source);
    } else if (!strcmp(method, "SAMPLE-AES")) {
        enc_type = ENC_AES_SAMPLE;
        me->enc_aes.iv_is_static = is_playlist_FPS(me->source);
    } else {
        return 1;
    }

    me->encryption = true;
    me->encryptiontype = enc_type;
    me->current_encryptiontype = enc_type;

    if (enc_type != ENC_AES128 && enc_type != ENC_AES_SAMPLE) {
        return 0;
    }

    if (uri) {
        char *link_to_key = strdup(uri);
        if (!link_to_key) {
            return 1;
        }
        extend_url(&link_to_key, me->url);
        free(me->enc_aes.key_url);
        me->enc_aes.key_url = link_to_key;
    }

    /* IV=0x... is a hexadecimal-sequence: [0-9A-F], lower case accepted too;
     * fewer than 32 digits = leading zeros */
    if (iv && iv[0] == '0' && (iv[1] == 'x' || iv[1] == 'X')) {
        const char *hex = iv + 2;
        size_t digits = strspn(hex, "0123456789abcdefABCDEF");
        if (digits > 0 && digits <= 2 * KEYLEN) {
            char iv_str[STRLEN_BTS(KEYLEN)];
            memset(iv_str, '0', 2 * KEYLEN);
            memcpy(iv_str + 2 * KEYLEN - digits, hex, digits);
            iv_str[2 * KEYLEN] = '\0';
            str_to_bin(me->enc_aes.iv_value, iv_str, KEYLEN);
            me->enc_aes.iv_is_static = true;
        }
    }
    return 0;
}

/* EXT-X-MAP: URI and BYTERANGE in any order. BYTERANGE is a quoted-string
 * here (RFC 8216 4.4.4.5), unlike the unquoted #EXT-X-BYTERANGE value. */
static int parse_map(hls_media_playlist_t *me, char *attrs)
{
    char *name = NULL;
    char *value = NULL;
    char *uri = NULL;
    char *range = NULL;

    while (next_tag_attrib(&attrs, &name, &value)) {
        if (!strcmp(name, "URI")) {
            uri = value;
        } else if (!strcmp(name, "BYTERANGE")) {
            range = value;
        }
    }
    if (!uri) {
        return 0;
    }

    hls_media_segment_t* map = calloc(1, sizeof(struct hls_media_segment));
    if (!map) {
        return 1;
    }
    map->url = strdup(uri);
    if (!map->url) {
        free(map);
        return 1;
    }
    map->is_map = true;
    map->size = -1;
    if (range) {
        parse_byterange(range, &map->offset, &map->size);
    }
    segment_list_append(&me->first_media_segment, &me->last_media_segment, map);
    return 0;
}

/* tag is one NUL-terminated playlist line (a copy), so the attribute searches
 * cannot run on into a later tag (e.g. take the IV of the next EXT-X-KEY) */
static int parse_tag(hls_media_playlist_t *me, struct hls_media_segment *ms, char *tag, int64_t *seg_offset, int64_t *seg_size)
{
    if (!strncmp(tag, "#EXT-X-KEY:", 11)) {
        return parse_key(me, tag + 11);
    } else if (!strncmp(tag, "#EXT-X-MAP:", 11)) {
        return parse_map(me, tag + 11);
    } else if (!strncmp(tag, "#EXTINF:", 8)) {
        ms->duration_ms = get_duration_ms(tag + 8);
    } else if (!strncmp(tag, "#EXT-X-ENDLIST", 14)) {
        me->is_endlist = true;
    } else if (!strncmp(tag, "#EXT-X-DISCONTINUITY-SEQUENCE:", 30)) {
        sscanf(tag + 30, "%d", &(me->discontinuity_sequence));
    } else if (!strncmp(tag, "#EXT-X-DISCONTINUITY", 20)) {
        ms->discontinuity = true;
    } else if (!strncmp(tag, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
        return sscanf(tag + 22, "%d", &(me->first_media_sequence)) == 1 ? 0 : 1;
    } else if (!strncmp(tag, "#EXT-X-TARGETDURATION:", 22)) {
        me->target_duration_ms = get_duration_ms(tag + 22);
    } else if (!strncmp(tag, "#EXT-X-BYTERANGE:", 17)) {
        parse_byterange(tag + 17, seg_offset, seg_size);
    } else {
        return 1;
    }
    return 0;
}

/* Each segment keeps the method, key and IV that were in force where it stands
 * in the playlist - a later EXT-X-KEY (key rotation, METHOD=NONE) must not
 * change how an earlier segment is decrypted. */
static void setup_segment_aes(hls_media_playlist_t *me, hls_media_segment_t *ms, int enc_type)
{
    ms->encryptiontype = enc_type;
    if (enc_type == ENC_AES128 || enc_type == ENC_AES_SAMPLE) {
        memcpy(ms->enc_aes.key_value, me->enc_aes.key_value, KEYLEN);
        memcpy(ms->enc_aes.iv_value, me->enc_aes.iv_value, KEYLEN);
        free(ms->enc_aes.key_url);
        /* an EXT-X-KEY without URI leaves no key to fetch */
        ms->enc_aes.key_url = me->enc_aes.key_url ? strdup(me->enc_aes.key_url) : NULL;
        if (me->enc_aes.iv_is_static == false) {
            /* RFC 8216 4.3.2.5 requires an explicit IV on the EXT-X-KEY that
             * applies to an EXT-X-MAP, so for the init segment this
             * sequence-number value is a best-effort guess for non-conformant
             * playlists. For a media segment it is the mandated IV. */
            char iv_str[STRLEN_BTS(KEYLEN)];
            uint8_t iv_bin[KEYLEN];
            snprintf(iv_str, STRLEN_BTS(KEYLEN), "%032x\n", ms->sequence_number);
            str_to_bin(iv_bin, iv_str, KEYLEN);
            memcpy(ms->enc_aes.iv_value, iv_bin, KEYLEN);
        }
    }
}

static int media_playlist_get_links(hls_media_playlist_t *me)
{
    struct hls_media_segment *map = NULL;
    struct hls_media_segment *ms = NULL;
    char *src = me->source;
    int64_t seg_offset = 0;
    int64_t seg_size = -1;

    MSG_PRINT("> START media_playlist_get_links\n");

    int i = 0;
    while(src != NULL){
        if (ms == NULL)
        {
            ms = malloc(sizeof(struct hls_media_segment));
            memset(ms, 0x00, sizeof(struct hls_media_segment));
        }

        while ((src = (strchr(src, '\n')))) {
            src++;
            if (*src == '\n') {
                continue;
            }
            if (*src == '\r') {
                continue;
            }
            if (*src == '#') {
                char *tag = strndup(src, strcspn(src, "\r\n"));
                if (tag) {
                    parse_tag(me, ms, tag, &seg_offset, &seg_size);
                    free(tag);
                }

                if (me->last_media_segment && me->last_media_segment->is_map
                        && me->last_media_segment != map) { // a new EXT-X-MAP was just parsed
                    map = me->last_media_segment;

                    map->sequence_number = i + me->first_media_sequence;
                    setup_segment_aes(me, map, me->current_encryptiontype);

                    /* Get full url */
                    extend_url(&(map->url), me->url);
                }
                
                continue;                    
            }
            if (*src == '\0') {
                goto finish;
            }

            /* the URL ends at \r or \n, or at the end of a playlist without a
             * final newline */
            ms->url = strndup(src, strcspn(src, "\r\n"));
            if (!ms->url) {
                goto finish;
            }

            ms->sequence_number = i + me->first_media_sequence;
            setup_segment_aes(me, ms, me->current_encryptiontype);

            /* Get full url */
            extend_url(&(ms->url), me->url);

            ms->size = seg_size;
            if (seg_size >= 0) {
                ms->offset = seg_offset;
                seg_offset += seg_size;
                seg_size = -1;
            } else {
                ms->offset = 0;
                seg_offset = 0;
            }

            /* Add new segment to segment list */
            segment_list_append(&(me->first_media_segment), &(me->last_media_segment), ms);
            ms = NULL;
            i += 1;
            break;
        }
    }

finish:

    if (i > 0) {
        me->last_media_sequence = me->first_media_sequence + i - 1;
    }

    /* An EXT-X-MAP that appeared before the EXT-X-KEY it belongs to was
     * parsed while encryption was still off, so setup_segment_aes() gave it
     * no key and it would be written out undecrypted. Back-fill from the
     * final key state - correct for the common single-key playlist; the
     * warning covers anything more exotic. */
    if (me->encryption) {
        struct hls_media_segment *s = me->first_media_segment;
        while (s) {
            if (s->is_map && s->encryptiontype == ENC_NONE && s->enc_aes.key_url == NULL) {
                MSG_WARNING("EXT-X-MAP before EXT-X-KEY - applying the playlist key to the init segment.\n");
                setup_segment_aes(me, s, me->encryptiontype);
            }
            s = s->next;
        }
    }

    media_segment_cleanup(ms);

    MSG_PRINT("> END media_playlist_get_links\n");

    return 0;
}

static uint64_t get_duration_hls_media_playlist(hls_media_playlist_t *me)
{
    uint64_t duration_ms = 0;
    struct hls_media_segment *ms = me->first_media_segment;
    while(ms) {
        duration_ms += ms->duration_ms;
        ms = ms->next;
    }
    return duration_ms;
}

int handle_hls_media_playlist(hls_media_playlist_t *me)
{
    me->encryption = false;
    me->encryptiontype = ENC_NONE;
    me->current_encryptiontype = ENC_NONE;

    if (!me->source) {
        size_t size = 0;
        long http_code = 0;
        int tries = hls_args.open_max_retries;

        while (tries) {
            http_code = get_hls_data_from_url(me->orig_url, &me->source, &size, STRING, &me->url);
            if (200 != http_code || size == 0) {
                MSG_ERROR("%s %d tries[%d]\n", me->orig_url, (int)http_code, (int)tries);
                --tries;
                sleep(1);
                continue;
            }
            break;
        }
    }

    me->first_media_segment = NULL;
    me->last_media_segment = NULL;
    me->target_duration_ms = 0;
    me->is_endlist = false;
    me->first_media_sequence = 0;
    me->last_media_sequence = 0;

    if (media_playlist_get_links(me)) {
        MSG_ERROR("Could not parse links. Exiting.\n");
        return 1;
    }
    me->total_duration_ms = get_duration_hls_media_playlist(me);
    return 0;
}

static bool get_next_attrib(char **source, char **tag, char **val)
{
    bool ret = false;
    char *ptr = NULL;
    char *token = NULL;
    char *value = NULL;
    char end_val_marker = '\0';
    char *src = *source;
    while (*src != '\0' && strchr(", \t\n\r", *src)) {
        ++src;
        continue;
    }

    ptr = src;
    while (*ptr != '=' && *ptr != '\0' && !strchr("\n\r", *ptr)) ++ptr;
    if (*ptr != '\0') {
        token = src;
        *ptr = '\0';

        ptr += 1;
        if (*ptr == '"') {
            ++ptr;
            end_val_marker = '"';
        } else {
            end_val_marker = ',';
        }

        value = ptr;
        while (*ptr != end_val_marker && *ptr != '\0' && !strchr("\n\r", *ptr)) ++ptr;
        src = ptr;
        if (*ptr) {
            ++src;
            *ptr = '\0';
        }

        if (*value) {
            *val = value;
            *tag = token;
            ret = true;
        }
        *source = src;
    } else {
        *source = ptr;
    }

    return ret;
}

int handle_hls_master_playlist(struct hls_master_playlist *ma)
{
    bool url_expected = false;
    unsigned int bitrate = 0;
    char *res = NULL;
    char *codecs = NULL;
    char *audio_grp = NULL;

    char *token = NULL;
    char *value = NULL;

    char *src = ma->source;
    while(*src != '\0'){
        char *end_ptr = strchr(src, '\n');
        if (!end_ptr) {
            /* last line without a newline - still a variant URL */
            end_ptr = src + strlen(src);
        }
        bool last_line = (*end_ptr == '\0');
        *end_ptr = '\0';
        /* CRLF playlist: the \r would end up in the variant URL */
        if (end_ptr > src && end_ptr[-1] == '\r') {
            end_ptr[-1] = '\0';
        }
        if (*src == '#') {
            url_expected = false;
            bitrate = 0;
            res = NULL;
            codecs = NULL;
            audio_grp = NULL;

            if (!strncmp(src, "#EXT-X-STREAM-INF:", 18)) {
                src += 18;
                while (get_next_attrib(&src, &token, &value)) {
                    if (!strncmp(token, "BANDWIDTH", 9)) {
                        sscanf(value, "%u", &bitrate);
                    } else if (!strncmp(token, "AUDIO", 5)) {
                        audio_grp = value;
                    } else if (!strncmp(token, "RESOLUTION", 10)) {
                        res = value;
                    } else if (!strncmp(token, "CODECS", 6)) {
                        codecs = value;
                    }
                }
                url_expected = true;
            } else if (!strncmp(src, "#EXT-X-MEDIA:TYPE=AUDIO,", 24)) {
                src += 24;
                char *grp_id = NULL;
                char *name = NULL;
                char *lang = NULL;
                char *url = NULL;
                bool is_default = false;

                while (get_next_attrib(&src, &token, &value)) {
                    if (!strncmp(token, "GROUP-ID", 8)) {
                        grp_id = value;
                    } else if (!strncmp(token, "NAME", 4)) {
                        name = value;
                    } else if (!strncmp(token, "LANGUAGE", 8)) {
                        lang = value;
                    } else if (!strncmp(token, "URI", 3)) {
                        url = value;
                    } else if (!strncmp(token, "DEFAULT", 7)) {
                        if (!strncmp(value, "YES", 3)) {
                            is_default = true;
                        }
                    }
                }

                if (grp_id && name && url) {
                    size_t len = strlen(url);

                    hls_audio_t *audio = malloc(sizeof(hls_audio_t));
                    memset(audio, 0x00, sizeof(hls_audio_t));
                    audio->url = malloc(len + 1);
                    memcpy(audio->url, url, len);
                    audio->url[len] = '\0';
                    extend_url(&(audio->url), ma->url);

                    audio->grp_id = strdup(grp_id);
                    audio->name = strdup(name);
                    audio->lang = lang ? strdup(lang) : NULL;
                    audio->is_default = is_default;

                    if (ma->audio) {
                        audio->next = ma->audio;
                    }
                    ma->audio = audio;
                }
            }
        } else if (url_expected) {
            size_t len = strlen(src);

            // here we will fill new playlist
            hls_media_playlist_t *me = malloc(sizeof(hls_media_playlist_t));
            memset(me, 0x00, sizeof(hls_media_playlist_t));

            me->url = malloc(len + 1);
            memcpy(me->url, src, len);
            me->url[len] = '\0';
            extend_url(&(me->url), ma->url);
            me->bitrate = bitrate;
            me->audio_grp = audio_grp ? strdup(audio_grp) : NULL;
            me->resolution = res ? strdup(res) : strdup("unknown");
            me->codecs = codecs ? strdup(codecs) : strdup("unknown");

            if (ma->media_playlist) {
                me->next = ma->media_playlist;
            }
            ma->media_playlist = me;

            url_expected = false;
        }

        src = last_line ? end_ptr : end_ptr + 1;
    }

    return 0;
}

static int sample_aes_append_av_data(ByteBuffer_t *out, ByteBuffer_t *in, const uint8_t *pcr, uint16_t pid, uint8_t *cont_count)
{
    uint8_t *av_data = in->data;
    uint32_t av_size = in->pos;

    uint8_t ts_header[4] = {TS_SYNC_BYTE, 0x40, 0x00, 0x10};
    ts_header[1] = ((pid >> 8) & 0x1F) | 0x40; // 0x40 - set payload_unit_start_indicator
    ts_header[2] = pid & 0xFF;

    uint8_t adapt_header[8] = {0x00};
    uint8_t adapt_header_size = 0;
    uint32_t payload_size = TS_PACKET_LENGTH - sizeof(ts_header);
    if (pcr[0] & 0x10) {
        adapt_header_size = 8;
        adapt_header[1] = pcr[0] & 0xF0; // set previus flags: discontinuity_indicator, random_access_indicator, elementary_stream_priority_indicator, PCR_flag
        memcpy(adapt_header + 2, pcr + 1, 6);
    } else if (pcr[0] & 0x20) {
        adapt_header_size = 2;
        adapt_header[1] = pcr[0] & 0xF0; // restore flags as described above
    } else if (av_size < payload_size) {
        adapt_header_size = payload_size - av_size == 1 ? 1 : 2;
    }

    payload_size -= adapt_header_size;

    if (adapt_header_size) {
        adapt_header[0] = adapt_header_size - 1; // size without field size
        if (av_size < payload_size) {
            adapt_header[0] += payload_size - av_size;
        }

        if (adapt_header[0]) {
            ts_header[3] = (ts_header[3] & 0xcf) | 0x30; // set addaptation filed flag
        }
    }

    // ts header
    ts_header[3] = (ts_header[3] & 0xf0) | (*cont_count);
    *cont_count = (*cont_count + 1) % 16;
    memcpy(&out->data[out->pos], ts_header, sizeof(ts_header));
    out->pos += sizeof(ts_header);

    // adaptation field
    if (adapt_header_size) {
        memcpy(&out->data[out->pos], adapt_header, adapt_header_size);
        out->pos += adapt_header_size;
    }

    if (av_size < payload_size) {
        uint32_t s;
        for(s=0; s < payload_size - av_size; ++s) {
            out->data[out->pos + s] = 0xff;
        }
        out->pos += payload_size - av_size;
        payload_size = av_size;
    }

    memcpy(&out->data[out->pos], av_data, payload_size);
    out->pos += payload_size;
    av_data += payload_size;
    av_size -= payload_size;

    if (av_size > 0) {
        uint32_t packets_num = av_size / (TS_PACKET_LENGTH - 4);
        uint32_t p;
        ts_header[1] &= 0xBF; // unset payload_unit_start_indicator
        ts_header[3] = (ts_header[3] & 0xcf) | 0x10; // unset addaptation filed flag
        for (p=0; p < packets_num; ++p) {
           ts_header[3] = (ts_header[3] & 0xf0) | (*cont_count);
           *cont_count = (*cont_count + 1) % 16;
           memcpy(&out->data[out->pos], ts_header, sizeof(ts_header));
           memcpy(&out->data[out->pos+4], av_data, TS_PACKET_LENGTH - sizeof(ts_header));
           out->pos += TS_PACKET_LENGTH;
           av_data += TS_PACKET_LENGTH - sizeof(ts_header);
           av_size -= TS_PACKET_LENGTH - sizeof(ts_header);
        }

        ts_header[3] = (ts_header[3] & 0xcf) | 0x30; // set addaptation filed flag to add aligment
        adapt_header[1] = 0; // none flags set, only for alignment
        if (av_size > 0) {
            ts_header[3] = (ts_header[3] & 0xf0) | (*cont_count);
            *cont_count = (*cont_count + 1) % 16;
            // add ts_header
            memcpy(&out->data[out->pos], ts_header, sizeof(ts_header));

            // add adapt header
            adapt_header_size = TS_PACKET_LENGTH - 4 - av_size == 1 ? 1 : 2;
            adapt_header[0] = adapt_header_size - 1; // size without field size
            if (adapt_header[0]) {
                adapt_header[0] +=  TS_PACKET_LENGTH - 4 - 2 - av_size;
            }

            memcpy(&out->data[out->pos+4], adapt_header, adapt_header_size);
            out->pos += 4 + adapt_header_size;

            // add alignment
            if (adapt_header[0]) {
                int32_t s;
                for(s=0; s < adapt_header[0] - 1; ++s) {
                    out->data[out->pos + s] = 0xff;
                }
                out->pos += adapt_header[0] -1;
            }

            // add payload
            memcpy(out->data + out->pos, av_data, av_size);
            out->pos += av_size;
        }
    }

    return 0;
}


static uint8_t * remove_emulation_prev(const uint8_t  *src,
                                       const uint8_t  *src_end,
                                             uint8_t  *dst,
                                             uint8_t  *dst_end)
{
    while (src + 2 < src_end)
        if (!*src && !*(src + 1) && *(src + 2) == 3) {
            *dst++ = *src++;
            *dst++ = *src++;
            src++; // remove emulation_prevention_three_byte
        } else
            *dst++ = *src++;

    while (src < src_end)
        *dst++ = *src++;

    return dst;
}

static int insert_emulation_prev(   const uint8_t   *src,
                                    const uint8_t   *src_end,
                                          uint8_t   *dst,
                                          uint8_t   *dst_end)
{
    int bytes_inserted = 0;
    while (src + 2 < src_end && dst + 3 < dst_end)
        if (!*src && !*(src + 1) && *(src + 2) < 3) {
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = 3; // insert emulation_prevention_three_byte
            *dst++ = *src++;
            bytes_inserted++;
        }
        else {
            *dst++ = *src++;
        }

    while (src < src_end)
        *dst++ = *src++;

    return bytes_inserted;
}

static uint8_t *ff_avc_find_startcode_internal(uint8_t *p, uint8_t *end)
{
    uint8_t *a = p + 4 - ((intptr_t)p & 3);

    for (end -= 3; p < a && p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for (end -= 3; p < end; p += 4) {
        uint32_t x = *(uint32_t*)p;
        if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
            if (p[1] == 0) {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p+1;
            }
            if (p[3] == 0) {
                if (p[2] == 0 && p[4] == 1)
                    return p+2;
                if (p[4] == 0 && p[5] == 1)
                    return p+3;
            }
        }
    }

    for (end += 3; p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

static uint8_t *ff_avc_find_startcode(uint8_t *p, uint8_t *end){
    uint8_t *out= ff_avc_find_startcode_internal(p, end);
    if(p<out && out<end && !out[-1]) out--;
    return out;
}

/* SAMPLE-AES restarts the CBC chain for every NAL unit / audio frame. One
 * cipher context, re-initialised each time, instead of allocating and freeing
 * one per frame. Used from the downloading thread only; lives until exit. */
static void *sample_aes_ctx(void)
{
    static void *ctx = NULL;
    if (!ctx) {
        ctx = AES128_CBC_CTX_new();
        if (!ctx) {
            MSG_ERROR("out of memory\n");
        }
    }
    return ctx;
}

static int sample_aes_decrypt_nal_units(hls_media_segment_t *s, uint8_t *buf_in, int size)
{
    uint8_t *end = buf_in + size;
    uint8_t *nal_start;
    uint8_t *nal_end;
    uint8_t* nal_new_start = NULL;
    int      nal_new_allocated = 0;
    end = remove_emulation_prev(buf_in, end, buf_in, end);

    nal_start = ff_avc_find_startcode(buf_in, end);
    for (;;) {
        while (nal_start < end && !*(nal_start++));
        if (nal_start == end)
            break;

        nal_end = ff_avc_find_startcode(nal_start, end);
        int nal_unit_type = *nal_start & 0x1F;
        int nal_size = nal_end - nal_start;
        // NAL unit with length of 48 bytes or fewer is completely unencrypted.
        void *ctx = sample_aes_ctx();
        if ((nal_unit_type == 1 || nal_unit_type == 5) && nal_size > 48 && ctx) {
            uint8_t* nal_start_bkup = nal_start;
            nal_start += 32;
            AES128_CBC_DecryptInit(ctx, s->enc_aes.key_value, s->enc_aes.iv_value, false);
            while (nal_start + 16 < nal_end) {
                AES128_CBC_DecryptUpdate(ctx, nal_start, nal_start, 16);
                nal_start += 16 * 10; // Each 16-byte block of encrypted data is followed by up to nine 16-byte blocks of unencrypted data.
            }
            nal_start = nal_start_bkup;
        }
        int bytes_inserted = 0;
        if (nal_size) {
            int nal_new_maxsize = nal_size * 4 / 3;
            if (nal_new_maxsize > nal_new_allocated) {
                uint8_t *tmp = realloc(nal_new_start, nal_new_maxsize);
                if (!tmp) {
                    MSG_ERROR("out of memory\n");
                    break;
                }
                nal_new_start = tmp;
                nal_new_allocated = nal_new_maxsize;
            }
            bytes_inserted = insert_emulation_prev(nal_start, nal_end, nal_new_start, nal_new_start + nal_new_maxsize);
        }
        if (bytes_inserted) {
            memmove(nal_end + bytes_inserted, nal_end, end - nal_end);
            memcpy(nal_start, nal_new_start, nal_size + bytes_inserted);
            nal_start = nal_end + bytes_inserted;
            end += bytes_inserted;
        }
        else
            nal_start = nal_end;
    }
    free(nal_new_start);
    return (int)(end - buf_in);
}

static int sample_aes_decrypt_audio_data(hls_media_segment_t *s, uint8_t *ptr, uint32_t size, audiotype_t audio_codec)
{
    bool (* get_next_frame)(const uint8_t **, const uint8_t *, uint32_t *);
    switch (audio_codec)
    {
    case AUDIO_ADTS:
        get_next_frame = adts_get_next_frame;
        break;
    case AUDIO_AC3:
        get_next_frame = ac3_get_next_frame;
        break;
    case AUDIO_EC3:
        get_next_frame = ec3_get_next_frame;
        break;
    case AUDIO_UNKNOWN:
    default:
        MSG_ERROR("Wrong audio_codec! Should never happen here > EXIT!\n");
        exit(1);
    }

    uint8_t *audio_frame = ptr;
    uint8_t *end_ptr = ptr + size;
    uint32_t frame_length = 0;
    while (get_next_frame((const uint8_t **)&audio_frame, end_ptr, &frame_length)) {
        // The IV must be reset at the beginning of every packet.
        uint8_t leaderSize = 0;

        if (audio_codec == AUDIO_ADTS) {
            // ADTS headers can contain CRC checks.
            // If the CRC check bit is 0, CRC exists.
            //
            // Header (7 or 9 byte) + unencrypted leader (16 bytes)
            leaderSize = (audio_frame[1] & 0x01) ? 23 : 25;
        } else { // AUDIO_AC3, AUDIO_EC3
            // AC3 Audio is untested. Sample streams welcome.
            //
            // unencrypted leader
            leaderSize = 16;
        }

        int tmp_size = frame_length > leaderSize ? (frame_length - leaderSize) & 0xFFFFFFF0  : 0;
        void *ctx = sample_aes_ctx();
        if (tmp_size && ctx) {
            AES128_CBC_DecryptInit(ctx, s->enc_aes.key_value, s->enc_aes.iv_value, false);
            AES128_CBC_DecryptUpdate(ctx, audio_frame + leaderSize, audio_frame + leaderSize, tmp_size);
        }

        audio_frame += frame_length;
    }

    return 0;
}


static int sample_aes_handle_pes_data(hls_media_segment_t *s, ByteBuffer_t *out, ByteBuffer_t *in, uint8_t *pcr, uint16_t pid, audiotype_t audio_codec, uint8_t *counter)
{
    uint16_t pes_header_size = 0;
    // we need to skip PES header it is not part of NAL unit
    if (in->pos <= PES_HEADER_SIZE || in->data[0] != 0x00 || in->data[1] != 0x00 || in->data[2] != 0x01) {
        MSG_ERROR("Wrong or missing PES header!\n");
        return -1;
    }

    pes_header_size = in->data[8] + 9;
    if (pes_header_size >= in->pos) {
        MSG_ERROR("Wrong PES header size %hu!\n", pes_header_size);
        return -1;
    }

    if (AUDIO_UNKNOWN == audio_codec) {
        // handle video data in NAL units
        int size = sample_aes_decrypt_nal_units(s, in->data + pes_header_size, in->pos - pes_header_size) + pes_header_size;

        // to check if I did not any mistake in offset calculation
        if (size > in->pos) {
            MSG_ERROR("NAL size after decryption is grater then before - before: %d, after: %d - should never happen!\n", size, in->pos);
            exit(-1);
        }

        // output size could be less then input because the start code emulation prevention could be removed if available
        if (size < in->pos) {
            // we need to update size in the PES header if it was set
            int32_t payload_size = ((uint16_t)(in->data[4]) << 8) | in->data[5];
            if (payload_size > 0) {
                payload_size -=  in->pos - size;
                in->data[4] = (payload_size >> 8) & 0xff;
                in->data[5] = payload_size & 0xff;
            }
            in->pos = size;
        }
    } else {
        sample_aes_decrypt_audio_data(s, in->data + pes_header_size, in->pos - pes_header_size, audio_codec);
    }

    return sample_aes_append_av_data(out, in, pcr, pid, counter);
}

/* 1: no key or no memory - the segment cannot be used; < 0: it is written
 * as it is (no PMT, no audio/video stream found) */
static int decrypt_sample_aes(hls_media_segment_t *s, ByteBuffer_t *buf)
{
    int ret = 0;
    /* without the key the "decrypted" data would be garbage */
    if (fill_key_value(&(s->enc_aes))) {
        return 1;
    }
    if (buf->len > TS_PACKET_LENGTH && buf->data[0] == TS_SYNC_BYTE) {
        pmt_data_t pmt = {0};
        if (find_pmt(buf->data, buf->len, &pmt)) {
            bool write_pmt = true;
            uint16_t audio_PID = PID_UNSPEC;
            uint16_t video_PID = PID_UNSPEC;
            audiotype_t audio_codec = AUDIO_UNKNOWN;
            uint32_t i;
            // https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/HLS_Sample_Encryption/TransportStreamSignaling/TransportStreamSignaling.html
            for (i=0; i < pmt.component_num; ++i) {
                uint8_t stream_type = pmt.components[i].stream_type;
                switch (stream_type) {
                    case 0xdb:
                        video_PID = pmt.components[i].elementary_PID;
                        stream_type = 0x1B; // AVC video stream as defined in ITU-T Rec. H.264 | ISO/IEC 14496-10 Video, or AVC base layer of an HEVC video stream as defined in ITU-T H.265 | ISO/IEC 23008-2
                        break;
                    case 0xcf:
                        audio_codec = AUDIO_ADTS;
                        audio_PID = pmt.components[i].elementary_PID;
                        stream_type = 0x0F; // ISO/IEC 13818-7 Audio with ADTS transport syntax
                        break;
                    case 0xc1:
                        audio_codec = AUDIO_AC3;
                        audio_PID = pmt.components[i].elementary_PID;
                        stream_type = 0x81; // User Private / AC-3 (ATSC)
                        break;
                    case 0xc2:
                        audio_codec = AUDIO_EC3;
                        audio_PID = pmt.components[i].elementary_PID;
                        stream_type = 0x87; // User Private / E-AC-3 (ATSC)
                        break;
                    default:
                        MSG_DBG("Unknown component type: 0x%02hhx, pid: 0x%03hx\n", pmt.components[i].stream_type, pmt.components[i].elementary_PID);
                        break;
                }

                if (stream_type != pmt.components[i].stream_type) {
                    // we update stream type to reflect unencrypted data
                    pmt.components[i].stream_type = stream_type;
                    pmt.data[pmt.components[i].offset] = stream_type;
                }
            }

            if (audio_PID != PID_UNSPEC || video_PID != PID_UNSPEC) {
                uint8_t audio_counter = 0;
                uint8_t video_counter = 0;
                uint8_t audio_pcr[7] = {0}; // first byte is adaptation filed flags
                uint8_t video_pcr[7] = {0}; // - || -
                ByteBuffer_t outBuffer = {NULL};
                outBuffer.data = malloc((size_t)buf->len * 4 / 3);
                outBuffer.len = buf->len;

                ByteBuffer_t audioBuffer = {NULL};
                ByteBuffer_t videoBuffer = {NULL};

                if (audio_PID != PID_UNSPEC) {
                    audioBuffer.data = malloc(buf->len);
                    audioBuffer.len = buf->len;
                }

                if (video_PID != PID_UNSPEC) {
                    videoBuffer.data = malloc((size_t)buf->len * 4 / 3);    // reserve space for emulation_prevention_three_byte
                    videoBuffer.len = buf->len;
                }

                /* the copies below write into these buffers unchecked */
                if (!outBuffer.data
                    || (audio_PID != PID_UNSPEC && !audioBuffer.data)
                    || (video_PID != PID_UNSPEC && !videoBuffer.data)) {
                    MSG_ERROR("out of memory\n");
                    free(outBuffer.data);
                    free(audioBuffer.data);
                    free(videoBuffer.data);
                    return 1;
                }

                // collect all audio and video data
                uint32_t packet_id = 0;
                uint8_t *ptr = buf->data;
                uint8_t *end = ptr + buf->len;
                while (ptr + TS_PACKET_LENGTH <= end) {
                    if (*ptr != TS_SYNC_BYTE) {
                        MSG_WARNING("Expected sync byte but got 0x%02hhx!\n", *ptr);
                        ptr += 1;
                        continue;
                    }
                    ts_packet_t packed = {0};
                    parse_ts_packet(ptr, &packed);

                    if (packed.pid == pmt.pid) {
                        if (write_pmt) {
                            write_pmt = false;
                            pmt_update_crc(&pmt);
                            memcpy(&outBuffer.data[outBuffer.pos], pmt.data, TS_PACKET_LENGTH);
                            outBuffer.pos += TS_PACKET_LENGTH;
                        }
                    } else if (packed.pid == audio_PID || packed.pid == video_PID) {
                        ByteBuffer_t *pCurrBuffer = packed.pid == audio_PID ? &audioBuffer : &videoBuffer;
                        uint8_t *pcr = packed.pid == audio_PID ? audio_pcr : video_pcr;
                        uint8_t *counter = packed.pid == audio_PID ? &audio_counter : &video_counter;

                        if (packed.unitstart) {
                            // consume previous data if any
                            if (pCurrBuffer->pos) {
                                sample_aes_handle_pes_data(s, &outBuffer, pCurrBuffer, pcr, packed.pid, packed.pid == audio_PID ? audio_codec : AUDIO_UNKNOWN, counter);
                            }

                            if ((packed.afc & 2) && (ptr[5] & 0x10)) { // remember PCR if available
                                memcpy(pcr, ptr + 4 + 1, 7);
                            } else if ((packed.afc & 2) && (ptr[5] & 0x20)) { // remember discontinuity_indicator if set
                                pcr[0] = ptr[5];
                            } else {
                                pcr[0] = 0;
                            }
                            pCurrBuffer->pos = 0;
                        }

                        if (packed.payload_offset < TS_PACKET_LENGTH) {
                            memcpy(&(pCurrBuffer->data[pCurrBuffer->pos]), ptr + packed.payload_offset, TS_PACKET_LENGTH - packed.payload_offset);
                            pCurrBuffer->pos += TS_PACKET_LENGTH - packed.payload_offset;
                        }
                    } else {
                        memcpy(&outBuffer.data[outBuffer.pos], ptr, TS_PACKET_LENGTH);
                        outBuffer.pos += TS_PACKET_LENGTH;
                    }

                    ptr += TS_PACKET_LENGTH;
                    packet_id += 1;
                }

                if (audioBuffer.pos) {
                    sample_aes_handle_pes_data(s, &outBuffer, &audioBuffer, audio_pcr, audio_PID, audio_codec, &audio_counter);
                }

                if (videoBuffer.pos) {
                    sample_aes_handle_pes_data(s, &outBuffer, &videoBuffer, video_pcr, video_PID, AUDIO_UNKNOWN, &video_counter);
                }

                if (outBuffer.pos > buf->len ) {
                    MSG_ERROR("decrypt_sample_aes - buffer overflow detected!\n");
                    exit(-1);
                }

                free(videoBuffer.data);
                free(audioBuffer.data);

                // replace encrypted data with decrypted one
                free(buf->data);
                buf->data = outBuffer.data;
                buf->len = outBuffer.pos;
            } else {
                MSG_WARNING("None audio nor video component found!\n");
                ret = -3;
            }
        } else {
            MSG_WARNING("PMT could not be found!\n");
            ret = -2;
        }
    } else {
        MSG_WARNING("Unknown segment type!\n");
        ret = -1;
    }

    return ret;
}

static int decrypt_aes128(hls_media_segment_t *s, ByteBuffer_t *buf)
{
    // The AES128 method encrypts whole segments.
    // Simply decrypting them is enough.
    // Without the key, or with data that is no whole number of cipher blocks
    // (cut transfer, or no ciphertext at all), the result is garbage.
    if (fill_key_value(&(s->enc_aes)) || buf->len <= 0 || buf->len % KEYLEN) {
        return 1;
    }

    void *ctx = AES128_CBC_CTX_new();
    /* some AES-128 encrypted segments could be not correctly padded
     * and decryption with padding will fail - example stream with such problem is welcome
     * From other hand dump correctly padded segment will contain trashes, which will cause many
     * errors during processing such TS, for example by DVBInspector,
     * if padding will be not removed.
     */
#if 1
    int out_size = 0;
    int decrypted = ctx && AES128_CBC_DecryptInit(ctx, s->enc_aes.key_value, s->enc_aes.iv_value, true);
    if (decrypted) {
        /* A bad padding block is only logged: the data before it is fine, and
         * such segments played before - failing the download on it would
         * break streams that work today. */
        AES128_CBC_DecryptPadded(ctx, buf->data, buf->data, buf->len, &out_size);
    }
    // decoded data size could be less then input because of the padding
    buf->len = out_size;
#else
    int decrypted = 1;
    AES128_CBC_DecryptInit(ctx, s->enc_aes.key_value, s->enc_aes.iv_value, false);
    AES128_CBC_DecryptUpdate(ctx, buf->data, buf->data, buf->len);
#endif
    AES128_CBC_free(ctx);
    /* nothing left after the padding is no segment either */
    return (decrypted && buf->len > 0) ? 0 : 1;
}

/* pthread_cleanup handler: pthread_cond_timedwait reacquires the mutex before
 * acting on a cancellation, so a plain pthread_cancel() on this thread would
 * leave media_playlist_mtx locked and the later pthread_mutex_destroy() would
 * return EBUSY. */
static void unlock_media_playlist_mtx(void *mtx)
{
    pthread_mutex_unlock((pthread_mutex_t *)mtx);
}

static void *hls_playlist_update_thread(void *arg)
{
#ifndef _MSC_VER
    char threadname[50];
    strncpy(threadname, __func__, sizeof(threadname));
    threadname[49] = '\0';
#if !defined(__APPLE__) && !defined(__MINGW32__) && !defined(__CYGWIN__)
    prctl(PR_SET_NAME, (unsigned long)&threadname);
#endif
#endif

    hls_playlist_updater_params *updater_params = arg;

    hls_media_playlist_t *me = updater_params->media_playlist;
    pthread_mutex_t *media_playlist_mtx         = (pthread_mutex_t *)(updater_params->media_playlist_mtx);

    pthread_cond_t  *media_playlist_refresh_cond = (pthread_cond_t *)(updater_params->media_playlist_refresh_cond);
    pthread_cond_t  *media_playlist_empty_cond   = (pthread_cond_t *)(updater_params->media_playlist_empty_cond);

    void *session = init_hls_session();
    set_timeout_session(session, 2L, 3L);
    bool is_endlist = false;
    //char *url = NULL;
    int refresh_delay_s = 0;

    // no lock is needed here because download_live_hls not change this fields
    //pthread_mutex_lock(media_playlist_mtx);
    is_endlist = me->is_endlist;
    if (hls_args.refresh_delay_sec < 0) {
        refresh_delay_s = (int)(me->target_duration_ms / 1000);
        //pthread_mutex_unlock(media_playlist_mtx);

        if (refresh_delay_s > HLSDL_MAX_REFRESH_DELAY_SEC) {
            refresh_delay_s = HLSDL_MAX_REFRESH_DELAY_SEC;
        } else if (refresh_delay_s < HLSDL_MIN_REFRESH_DELAY_SEC) {
            refresh_delay_s = HLSDL_MIN_REFRESH_DELAY_SEC;
        }
    } else {
        refresh_delay_s = hls_args.refresh_delay_sec;
    }

    struct timespec ts;
    memset(&ts, 0x00, sizeof(ts));
    MSG_VERBOSE("Update thread started\n");
    while (!is_endlist) {
        // download live hls can interrupt waiting
        ts.tv_sec =  time(NULL) + refresh_delay_s;
        bool stop = false;
        pthread_mutex_lock(media_playlist_mtx);
        pthread_cleanup_push(unlock_media_playlist_mtx, media_playlist_mtx);
        if (!updater_params->stop) {
            pthread_cond_timedwait(media_playlist_refresh_cond, media_playlist_mtx, &ts);
        }
        stop = updater_params->stop;
        pthread_cleanup_pop(1);   /* unlock; also runs if cancelled in the wait */
        if (stop) {
            break;
        }

        // update playlist
        hls_media_playlist_t new_me;
        memset(&new_me, 0x00, sizeof(new_me));

        size_t size = 0;
        MSG_PRINT("> START DOWNLOAD LIST url[%s]\n", me->url);
        long http_code = get_data_from_url_with_session(&session, me->url, &new_me.source, &size, STRING, &(new_me.url), -1, -1);
        MSG_PRINT("> END DOWNLOAD LIST\n");
        if (200 == http_code && 0 == media_playlist_get_links(&new_me)) {
            // no mutex is needed here because download_live_hls not change this fields
            if (new_me.is_endlist ||
                new_me.first_media_sequence != me->first_media_sequence ||
                new_me.last_media_sequence != me->last_media_sequence)
            {
                bool list_extended = false;
                // we need to update list
                pthread_mutex_lock(media_playlist_mtx);
                me->is_endlist = new_me.is_endlist;
                is_endlist = new_me.is_endlist;
                me->first_media_sequence = new_me.first_media_sequence;

                if (new_me.last_media_sequence > me->last_media_sequence)
                {
                    // add new segments
                    struct hls_media_segment *ms = new_me.first_media_segment;
                    while (ms) {
                        /* Only splice in genuinely new media segments -
                         * re-queuing the whole window on every refresh would
                         * re-download it all. An EXT-X-MAP right in front of
                         * the first new segment is the init those segments
                         * need (a new one after a discontinuity) and goes
                         * along; the writer drops it when it is the init it
                         * already wrote. Maps further on come with the rest
                         * of the list. */
                        if (!ms->is_map && ms->sequence_number > me->last_media_sequence) {
                            if (ms->prev && ms->prev->is_map) {
                                ms = ms->prev;
                            }
                            if (ms->prev) {
                                ms->prev->next = NULL;
                            }
                            ms->prev = NULL;

                            if (me->last_media_segment) {
                                /* keep the queue doubly linked */
                                ms->prev = me->last_media_segment;
                                me->last_media_segment->next = ms;
                            } else {
                                assert(me->first_media_segment == NULL);
                                me->first_media_segment = ms;
                            }

                            me->last_media_segment = new_me.last_media_segment;
                            me->last_media_sequence = new_me.last_media_sequence;

                            if (ms == new_me.first_media_segment) {
                                // all segments are new
                                new_me.first_media_segment = NULL;
                                new_me.last_media_segment = NULL;
                            }

                            while (ms) {
                                me->total_duration_ms += ms->duration_ms;
                                ms = ms->next;
                            }

                            list_extended = true;
                            break;
                        }

                        ms = ms->next;
                    }
                }
                /* an ENDLIST without new segments must wake the downloader
                 * too, or it waits forever on an empty queue after this thread
                 * has ended */
                if (list_extended || is_endlist) {
                    pthread_cond_signal(media_playlist_empty_cond);
                }
                pthread_mutex_unlock(media_playlist_mtx);
            }
        } else {
            MSG_WARNING("Fail to update playlist \"%s\". http_code[%d].\n", me->url, (int)http_code);
            clean_http_session(session);
            sleep(1);
            session = init_hls_session();
            set_timeout_session(session, 2L, 15L);
            set_fresh_connect_http_session(session, 1);
        }
        media_playlist_cleanup(&new_me);
    }

    clean_http_session(session);
    pthread_exit(NULL);
    return NULL;
}

/* An fMP4 / CMAF segment starts with an ISO-BMFF box: <4-byte size><4-byte
 * type>. A TS segment starts with the 0x47 sync byte. */
static bool buffer_is_fmp4(const struct ByteBuffer *buf)
{
    if (!buf || !buf->data || buf->len < 8) {
        return false;
    }
    const uint8_t *d = (const uint8_t *)buf->data;
    if (d[0] == 0x47) {
        return false;   /* MPEG-2 TS sync byte - definitely not fMP4 */
    }
    const uint8_t *t = d + 4;
    return !memcmp(t, "ftyp", 4) || !memcmp(t, "styp", 4)
        || !memcmp(t, "moof", 4) || !memcmp(t, "moov", 4)
        || !memcmp(t, "sidx", 4) || !memcmp(t, "emsg", 4);
}

/* Called once, after the first EXT-X-MAP or media segment has been fetched.
 * Sets me->media_type and rejects the combinations hlsdl cannot produce a
 * usable file for. Returns non-zero to abort the download. */
static int detect_media_type(hls_media_playlist_t *me, const struct ByteBuffer *first)
{
    if (me->media_type != MEDIA_TYPE_UNKNOWN) {
        return 0;
    }
    me->media_type = buffer_is_fmp4(first) ? MEDIA_TYPE_FMP4 : MEDIA_TYPE_TS;

    if (me->media_type == MEDIA_TYPE_FMP4) {
        MSG_VERBOSE("Fragmented MP4 stream.\n");
        if (me->encryption && (me->encryptiontype == ENC_AES_SAMPLE
                            || me->encryptiontype == ENC_AES_SAMPLE_CTR)) {
            MSG_ERROR("SAMPLE-AES encrypted fragmented MP4 is not supported by "
                      "hlsdl - use a remuxer (e.g. ffmpeg).\n");
            MSG_API("{\"error_code\":-1, \"error_msg\":\"fmp4-sample-aes\"}\n");
            return 1;
        }
    }
    return 0;
}

/* Some CDNs disguise MPEG-TS segments as images: a small PNG/JPEG/GIF/WEBP
 * sits in front of the TS data (e.g. live streams whose segments are served
 * from an image CDN). Written as-is, the output starts with the image and no
 * player opens it. Only a segment that starts with an image signature AND has
 * a run of TS packets within the first 64 KB is cut at the first packet -
 * anything else (plain TS, fMP4, unknown data) is left untouched. The cut runs
 * on the plaintext: an image in front of AES-128 ciphertext breaks the
 * decryption before this point and cannot be repaired here. */
#define DISGUISE_SEARCH_LEN (64 * 1024)
/* packets in a row that mark the TS start - more than the usual three, since
 * this cuts real data and image bytes may hold 0x47 by chance */
#define DISGUISE_PACKET_RUN 5

static void strip_disguise_prefix(struct ByteBuffer *seg)
{
    static bool warned = false;

    if (!seg || !seg->data || seg->len < 12 || seg->data[0] == TS_SYNC_BYTE) {
        return;
    }
    const uint8_t *d = seg->data;
    bool image = !memcmp(d, "\x89PNG\r\n\x1a\n", 8)
              || (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
              || !memcmp(d, "GIF8", 4)
              || (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4));
    if (!image) {
        return;
    }

    /* look for the packet run in the head only, image data practically never
     * holds one */
    int limit = seg->len;
    if (limit > DISGUISE_SEARCH_LEN + 3 * TS_PACKET_LENGTH) {
        limit = DISGUISE_SEARCH_LEN + 3 * TS_PACKET_LENGTH;
    }
    struct ByteBuffer head = *seg;
    head.len = limit;
    uint8_t *first;
    while ((first = find_first_ts_packet(&head)) != NULL) {
        size_t left = (size_t)(seg->len - (first - seg->data));
        if (consecutive_sync_byte(first, left, DISGUISE_PACKET_RUN)) {
            break;
        }
        head.data = first + 1;
        head.len = limit - (int)(head.data - seg->data);
    }
    if (first == NULL) {
        return;
    }
    int skip = (int)(first - seg->data);
    memmove(seg->data, first, seg->len - skip);
    seg->len -= skip;
    if (!warned) {
        MSG_WARNING("Segments are disguised as images - cutting %d bytes in front of the MPEG-TS data.\n", skip);
        warned = true;
    }
}

/* A response that came back fine but cannot be this segment. A server that
 * ignores the Range header answers 200 with the whole file: an init segment
 * with BYTERANGE must be exactly that range, and a media segment with
 * BYTERANGE must not be larger (it would put the whole file into the output
 * once per segment). A segment over 2 GB does not fit the buffer length.
 * Retrying changes none of this, so the caller does not retry. (An empty body
 * is handled like a failed transfer and is retried.) */
static bool unusable_segment_response(const struct hls_media_segment *ms, long http_code, size_t size)
{
    if (size > INT_MAX) {
        return true;
    }
    if (ms->size < 0) {
        return false;
    }
    if (!ms->is_map) {
        return size > (size_t)ms->size;
    }
    return size != (size_t)ms->size || (strstr(ms->url, "://") && http_code != 206);
}

/* The init segment (EXT-X-MAP) last written to the output. Packagers repeat
 * the tag - after every discontinuity, around ad breaks, sometimes with a new
 * token in the URL - and a second ftyp/moov in the middle of an fMP4 file
 * breaks playback. A map is written only when it differs from this one: by
 * its source (URL + byte range, checked before the download) or, failing
 * that, by its plaintext bytes. */
typedef struct written_map {
    char *url;
    int64_t offset;
    int64_t size;
    uint8_t *data;   /* NULL when the bytes are unknown (skipped by -R) */
    int len;
} written_map_t;

static bool written_map_same_source(const written_map_t *w, const struct hls_media_segment *ms)
{
    return w->url && ms->url && !strcmp(w->url, ms->url)
        && w->offset == ms->offset && w->size == ms->size;
}

static bool written_map_same_bytes(const written_map_t *w, const struct ByteBuffer *seg)
{
    return w->data && seg->data && w->len == seg->len && !memcmp(w->data, seg->data, seg->len);
}

/* seg may be NULL: the source is known, the bytes are not */
static void written_map_set(written_map_t *w, const struct hls_media_segment *ms, const struct ByteBuffer *seg)
{
    free(w->url);
    free(w->data);
    w->url = ms->url ? strdup(ms->url) : NULL;
    w->offset = ms->offset;
    w->size = ms->size;
    w->data = NULL;
    w->len = 0;
    if (seg && seg->data && seg->len > 0) {
        w->data = malloc(seg->len);
        if (w->data) {
            memcpy(w->data, seg->data, seg->len);
            w->len = seg->len;
        }
    }
}

static void written_map_free(written_map_t *w)
{
    free(w->url);
    free(w->data);
    memset(w, 0, sizeof(*w));
}

/* Where merge_packets() takes an audio segment from: packed audio (raw
 * AAC / AC-3 behind an ID3 tag with the timestamp) as it is - merge_packets()
 * packs it into TS itself, and looking for TS packets in it would find none,
 * so the audio was dropped - and a TS segment from its first packet. */
static uint8_t *audio_merge_start(ByteBuffer_t *seg)
{
    if (seg->len >= 10 && 0 == memcmp(seg->data, "ID3", 3)) {
        /* An ID3 tag can also sit in front of a TS segment (timed
         * metadata) - only raw audio behind the tag is packed audio. The
         * tag size is syncsafe (7 bits per byte), a footer adds 10 bytes. */
        const uint8_t *d = seg->data;
        size_t tag = 10 + (((size_t)(d[6] & 0x7f) << 21) | ((size_t)(d[7] & 0x7f) << 14)
                         | ((size_t)(d[8] & 0x7f) << 7) | (size_t)(d[9] & 0x7f));
        if (d[5] & 0x10) {
            tag += 10;
        }
        if (tag < (size_t)seg->len && seg->data[tag] == TS_SYNC_BYTE
            && consecutive_sync_byte(seg->data + tag, (size_t)seg->len - tag, 3)) {
            return seg->data + tag;
        }
        return seg->data;
    }
    return find_first_ts_packet(seg);
}

/* Live audio rendition (EXT-X-MEDIA TYPE=AUDIO, or -a): consecutive audio
 * segments that could not be had before the audio track is given up and the
 * video goes on alone. Waiting for and fetching the audio of one segment
 * share one deadline (audio_wait_sec): an audio CDN that is down must not
 * hold up the video until the live window has moved on. */
#define LIVE_AUDIO_MAX_MISSES 3

static void unlink_queue_head(hls_media_playlist_t *pl)
{
    struct hls_media_segment *head = pl->first_media_segment;
    pl->first_media_segment = head->next;
    if (pl->first_media_segment) {
        pl->first_media_segment->prev = NULL;
    } else {
        /* the refresh thread appends through last_media_segment */
        pl->last_media_segment = NULL;
    }
    head->next = NULL;
}

/* Takes the audio segment with media sequence number seq off the audio queue,
 * dropping older ones and audio init segments on the way. NULL when it is
 * already gone or does not show up before the deadline (the playlist refresh
 * is asked for while waiting). Called without the mutex held. */
static struct hls_media_segment *live_take_audio(hls_media_playlist_t *ma, int seq, pthread_mutex_t *mtx,
                                                 pthread_cond_t *refresh_cond, pthread_cond_t *empty_cond, time_t deadline)
{
    struct hls_media_segment *found = NULL;
    struct timespec ts;
    memset(&ts, 0x00, sizeof(ts));
    ts.tv_sec = deadline;

    pthread_mutex_lock(mtx);
    while (true) {
        struct hls_media_segment *as = ma->first_media_segment;
        if (as) {
            if (as->is_map || as->sequence_number < seq) {
                unlink_queue_head(ma);
                media_segment_cleanup(as);
                continue;
            }
            if (as->sequence_number == seq) {
                unlink_queue_head(ma);
                found = as;
            }
            break;   /* found it, or it is already gone */
        }
        if (ma->is_endlist) {
            break;
        }
        pthread_cond_broadcast(refresh_cond);
        if (ETIMEDOUT == pthread_cond_timedwait(empty_cond, mtx, &ts)) {
            break;
        }
    }
    pthread_mutex_unlock(mtx);
    return found;
}

/* Fetches and decrypts one live audio segment - the video waits meanwhile, so
 * a failed transfer is retried only while the deadline leaves time for it.
 * 0: seg holds the plaintext. */
static int live_fetch_audio(void **psession, struct hls_media_segment *as, struct ByteBuffer *seg, time_t deadline)
{
    int tries = 0;
    while (true) {
        memset(seg, 0x00, sizeof(*seg));
        size_t size = 0;
        long http_code = get_data_from_url_with_session(psession, as->url, (char **)&(seg->data), &size, BINARY, NULL, as->offset, as->size);
        seg->len = (int)size;
        bool http_ok = http_code == 200 || (http_code == 206 && (as->size > -1 || hls_args.accept_partial_content));
        if (http_ok && size > 0 && !unusable_segment_response(as, http_code, size)) {
            if (tries) {
                set_fresh_connect_http_session(*psession, 0);
            }
            bool failed = (as->encryptiontype == ENC_AES128 && 0 != decrypt_aes128(as, seg));
            if (!failed) {
                strip_disguise_prefix(seg);
                failed = (as->encryptiontype == ENC_AES_SAMPLE && 1 == decrypt_sample_aes(as, seg));
            }
            if (!failed) {
                return 0;
            }
            free(seg->data);
            seg->data = NULL;
            return 1;
        }
        free(seg->data);
        seg->data = NULL;
        if (http_ok || http_code == 403 || http_code == 401 || http_code == 410
            || tries >= 2 || time(NULL) + 2 > deadline) {
            MSG_WARNING("Live mode skipping audio segment %d. http_code[%d].\n", as->sequence_number, (int)http_code);
            return 1;
        }
        clean_http_session(*psession);
        sleep(1);
        *psession = init_hls_session();
        set_timeout_session(*psession, 2L, 5L);
        set_fresh_connect_http_session(*psession, 1);
        tries += 1;
    }
}

/* Ends a live updater thread started with stop = false. */
static void stop_updater_thread(hls_playlist_updater_params *params, pthread_t thread)
{
    pthread_mutex_lock((pthread_mutex_t *)params->media_playlist_mtx);
    params->stop = true;
    pthread_cond_broadcast((pthread_cond_t *)params->media_playlist_refresh_cond);
    pthread_mutex_unlock((pthread_mutex_t *)params->media_playlist_mtx);
    pthread_join(thread, NULL);
}

int download_live_hls(write_ctx_t *out_ctx, hls_media_playlist_t *me, hls_media_playlist_t *me_audio)
{
    MSG_API("{\"d_t\":\"live\"}\n");

    if (hls_args.resume) {
        MSG_WARNING("-R (resume) has no effect on a live stream.\n");
    }

    hls_playlist_updater_params updater_params;

    /* declaration synchronization prymitives */
    pthread_mutex_t media_playlist_mtx;
    pthread_mutex_t cookie_file_mtx;

    pthread_cond_t  media_playlist_refresh_cond;
    pthread_cond_t  media_playlist_empty_cond;

    /* init synchronization prymitives */
    pthread_mutex_init(&media_playlist_mtx, NULL);
    pthread_mutex_init(&cookie_file_mtx, NULL);

    pthread_cond_init(&media_playlist_refresh_cond, NULL);
    pthread_cond_init(&media_playlist_empty_cond, NULL);

    memset(&updater_params, 0x00, sizeof(updater_params));
    updater_params.media_playlist = me;
    updater_params.media_playlist_mtx = (void *)&media_playlist_mtx;
    updater_params.media_playlist_refresh_cond = (void *)&media_playlist_refresh_cond;
    updater_params.media_playlist_empty_cond   = (void *)&media_playlist_empty_cond;

    hls_args.cookie_file_mutex = (void *)&cookie_file_mtx;

    // skip first segments
    if (me->first_media_segment != me->last_media_segment) {
        /* An EXT-X-MAP init segment sits at the head with duration 0 and is
         * not re-queued by the refresh path - detach it before trimming so
         * it does not get freed with the skipped media segments. */
        struct hls_media_segment *map = NULL;
        if (me->first_media_segment->is_map) {
            map = me->first_media_segment;
            me->first_media_segment = map->next;
            me->first_media_segment->prev = NULL;
        }

        if (me->first_media_segment != me->last_media_segment) {
            struct hls_media_segment *ms = me->last_media_segment;
            uint64_t duration_ms = 0;
            uint64_t duration_offset_ms = hls_args.live_start_offset_sec * 1000;
            while (ms) {
                duration_ms += ms->duration_ms;
                if (duration_ms >= duration_offset_ms) {
                    break;
                }
                ms = ms->prev;
            }

            if (ms && ms != me->first_media_segment){
                // remove segments
                while (me->first_media_segment != ms) {
                    struct hls_media_segment *tmp_ms = me->first_media_segment;
                    me->first_media_segment = me->first_media_segment->next;
                    if (tmp_ms->is_map) {
                        /* a later EXT-X-MAP in the skipped part (new init
                         * after a discontinuity) is the one in force where
                         * the download starts - it replaces the head map */
                        media_segment_cleanup(map);
                        map = tmp_ms;
                        map->prev = NULL;
                        map->next = NULL;
                    } else {
                        media_segment_cleanup(tmp_ms);
                    }
                }
                ms->prev = NULL;
                me->first_media_segment = ms;
            }
        }

        if (map) {
            map->next = me->first_media_segment;
            me->first_media_segment->prev = map;
            me->first_media_segment = map;
        }

        me->total_duration_ms = get_duration_hls_media_playlist(me);
    }

    /* Separate audio rendition: its own refresh thread on the same mutex and
     * conditions, its segments paired with the video ones by media sequence
     * number and muxed in like the VOD path does (TS only). Renditions of one
     * stream normally share the numbering. The two playlists were fetched one
     * after the other, so their windows may be a segment or so apart - that
     * is no reason to shift the pairing (it would put every segment one
     * segment out of sync); only windows that do not overlap at all are taken
     * as numbered apart, and their live edges are lined up instead. */
    bool audio_on = me_audio && me_audio->first_media_segment;
    hls_playlist_updater_params audio_params;
    pthread_t audio_thread;
    int audio_seq_offset = 0;
    int audio_misses = 0;
    int audio_wait_sec = 0;
    void *audio_session = NULL;
    merge_context_t live_merge;
    memset(&audio_params, 0x00, sizeof(audio_params));
    memset(&live_merge, 0x00, sizeof(live_merge));
    if (audio_on) {
        if (me->last_media_sequence < me_audio->first_media_sequence
            || me_audio->last_media_sequence < me->first_media_sequence) {
            audio_seq_offset = me_audio->last_media_sequence - me->last_media_sequence;
            MSG_VERBOSE("Audio playlist numbered apart from the video - offset %d.\n", audio_seq_offset);
        }
        audio_wait_sec = 2 * (int)(me_audio->target_duration_ms / 1000) + 2;
        if (audio_wait_sec < 4) {
            audio_wait_sec = 4;
        } else if (audio_wait_sec > 30) {
            audio_wait_sec = 30;
        }
        live_merge.out = out_ctx;
        audio_params.media_playlist = me_audio;
        audio_params.media_playlist_mtx = (void *)&media_playlist_mtx;
        audio_params.media_playlist_refresh_cond = (void *)&media_playlist_refresh_cond;
        audio_params.media_playlist_empty_cond   = (void *)&media_playlist_empty_cond;
        audio_session = init_hls_session();
        set_timeout_session(audio_session, 2L, 3L);
        if (0 != pthread_create(&audio_thread, NULL, hls_playlist_update_thread, &audio_params)) {
            MSG_WARNING("Could not start the audio playlist refresh - writing the video only.\n");
            audio_on = false;
        }
    }
    bool audio_thread_running = audio_on;
    /* a discontinuity was crossed since the last merge - kept until the next
     * merge, also when the flagged segment itself is skipped */
    bool merge_reset_pending = false;

    // start update thread
    pthread_t thread;
    void *ret;

    pthread_create(&thread, NULL, hls_playlist_update_thread, &updater_params);

    void *session = init_hls_session();
    set_timeout_session(session, 2L, 3L);
    written_map_t written_map = {0};
    uint64_t downloaded_duration_ms = 0;
    int64_t download_size = 0;
    time_t repTime = 0;
    bool download = true;
    int result = 0;

    while(download) {
        pthread_mutex_lock(&media_playlist_mtx);
        struct hls_media_segment *ms = me->first_media_segment;
        if (ms != NULL) {
            me->first_media_segment = ms->next;
            if (me->first_media_segment) {
                me->first_media_segment->prev = NULL;
            } else {
                /* ms was the tail: clear last_media_segment in the same
                 * critical section, otherwise the refresh thread can write
                 * through it (me->last_media_segment->next = ...) after
                 * loop_cleanup has freed this node - a use-after-free that
                 * also exists upstream. */
                me->last_media_segment = NULL;
            }
        }
        else {
            me->last_media_segment = NULL;
            download = !me->is_endlist;
        }
        if (ms == NULL) {
            if (download) {
                /* broadcast: the audio refresh thread waits on it too */
                pthread_cond_broadcast(&media_playlist_refresh_cond);
                pthread_cond_wait(&media_playlist_empty_cond, &media_playlist_mtx);
            }
        }
        pthread_mutex_unlock(&media_playlist_mtx);
        if (ms == NULL) {
            continue;
        }

        if (ms->discontinuity) {
            merge_reset_pending = true;
        }

        int retries = 0;

        if (ms->is_map) {
            // don't duplicate the init segment if it is the one already written
            if (written_map_same_source(&written_map, ms))
                goto loop_cleanup;

            MSG_PRINT("Downloading init segment %s\n", ms->url);
        } else
            MSG_PRINT("Downloading part %d\n", ms->sequence_number);

        do {
            struct ByteBuffer seg;
            memset(&seg, 0x00, sizeof(seg));
            size_t size = 0;
            long http_code = get_data_from_url_with_session(&session, ms->url, (char **)&(seg.data), &size, BINARY, NULL, ms->offset, ms->size);
            seg.len = (int)size;
            /* an empty body counts as a failed transfer */
            if (!(http_code == 200 || (http_code == 206 && (ms->size > -1 || hls_args.accept_partial_content))) || size == 0) {
                int first_media_sequence = 0;
                if (seg.data) {
                    free(seg.data);
                    seg.data  = NULL;
                }

                pthread_mutex_lock(&media_playlist_mtx);
                first_media_sequence = me->first_media_sequence;
                pthread_mutex_unlock(&media_playlist_mtx);

                if (http_code != 403 && http_code != 401 && http_code != 410
                        && retries < hls_args.segment_download_retries
                        && (ms->sequence_number > first_media_sequence || ms->is_map)) {
                    clean_http_session(session);
                    sleep(1);
                    session = init_hls_session();
                    set_timeout_session(session, 2L, 5L);
                    set_fresh_connect_http_session(session, 1);
                    MSG_WARNING("Live retry segment %d download, due to previous error. http_code[%d].\n", ms->sequence_number, (int)http_code);
                    retries += 1;
                    continue;
                }
                MSG_WARNING("Live mode skipping segment %d. http_code[%d].\n", ms->sequence_number, (int)http_code);
                break;
            }
            if (unusable_segment_response(ms, http_code, size)) {
                MSG_WARNING("Live mode skipping segment %d - the response does not match its byte range.\n", ms->sequence_number);
                free(seg.data);
                break;
            }

            if (ms->discontinuity) {
                MSG_WARNING("Crossing EXT-X-DISCONTINUITY at segment %d - output may not be seamless.\n", ms->sequence_number);
            }

            /* AES-128 encrypts the whole segment, so the container sniff has to
             * run on the plaintext. An image head in front of the TS data is cut
             * off before the sniff. SAMPLE-AES leaves the box/PES headers in the
             * clear - sniff (and reject fMP4) before decrypt_sample_aes touches
             * the buffer. A segment that cannot be decrypted (key server
             * hiccup) is skipped like one that cannot be fetched - stopping a
             * live stream for it would be worse; VOD fails instead, so the
             * download can be resumed. */
            if (ms->encryptiontype == ENC_AES128 && 0 != decrypt_aes128(ms, &seg)) {
                MSG_WARNING("Live mode skipping segment %d - it could not be decrypted.\n", ms->sequence_number);
                free(seg.data);
                break;
            }
            if (!ms->is_map) {
                strip_disguise_prefix(&seg);
            }

            if (0 != detect_media_type(me, &seg)) {
                free(seg.data);
                download = false;
                result = 1;
                pthread_cancel(thread);
                break;
            }

            if (audio_on && me->media_type == MEDIA_TYPE_FMP4) {
                MSG_WARNING("Separate audio track with fragmented MP4 - hlsdl writes the video track only; use a remuxer for muxed output.\n");
                audio_on = false;
                stop_updater_thread(&audio_params, audio_thread);
                audio_thread_running = false;
            }

            if (ms->encryptiontype == ENC_AES_SAMPLE && 1 == decrypt_sample_aes(ms, &seg)) {
                MSG_WARNING("Live mode skipping segment %d - it could not be decrypted.\n", ms->sequence_number);
                free(seg.data);
                break;
            }

            if (ms->is_map && written_map_same_bytes(&written_map, &seg)) {
                /* same init under a new URL (e.g. a fresh token) */
                written_map_set(&written_map, ms, &seg);
                free(seg.data);
                break;
            }

            downloaded_duration_ms += ms->duration_ms;
            if (hls_args.live_duration_sec > 0 && downloaded_duration_ms > hls_args.live_duration_sec * 1000) {
                free(seg.data);
                download = false;
                pthread_cancel(thread);
                break;
            }

            /* the audio of this segment, muxed into its TS packets */
            size_t merged = 0;
            if (audio_on && !ms->is_map) {
                time_t audio_deadline = time(NULL) + audio_wait_sec;
                struct hls_media_segment *as = live_take_audio(me_audio, ms->sequence_number + audio_seq_offset,
                                                               &media_playlist_mtx, &media_playlist_refresh_cond,
                                                               &media_playlist_empty_cond, audio_deadline);
                struct ByteBuffer seg_audio;
                memset(&seg_audio, 0x00, sizeof(seg_audio));
                /* new PIDs / PMT after a discontinuity (ad break): the merge
                 * works out its PMT again (also when this segment ends up
                 * without audio - the next merge then starts afresh) */
                if (merge_reset_pending || (as && as->discontinuity)) {
                    merge_context_reset(&live_merge);
                    merge_reset_pending = false;
                }
                if (as && 0 == live_fetch_audio(&audio_session, as, &seg_audio, audio_deadline)) {
                    uint8_t *v = find_first_ts_packet(&seg);
                    uint8_t *a = audio_merge_start(&seg_audio);
                    if (v && a) {
                        merged = merge_packets(&live_merge, v, (uint32_t)(seg.len - (v - seg.data)),
                                               a, (uint32_t)(seg_audio.len - (a - seg_audio.data)));
                    }
                }
                free(seg_audio.data);
                media_segment_cleanup(as);

                if (merged) {
                    audio_misses = 0;
                } else {
                    MSG_WARNING("No audio for segment %d - writing the video only.\n", ms->sequence_number);
                    if (++audio_misses >= LIVE_AUDIO_MAX_MISSES) {
                        MSG_WARNING("The audio track keeps failing - going on with the video only.\n");
                        audio_on = false;
                        stop_updater_thread(&audio_params, audio_thread);
                        audio_thread_running = false;
                    }
                }
            }

            /* merge_packets() does not tell a short write apart; a full
             * disk then shows up with the next video-only write or at close */
            size_t written = merged ? (size_t)seg.len : out_ctx->write(seg.data, seg.len, out_ctx->opaque);
            download_size += merged ? merged : written;
            if (ms->is_map && written == (size_t)seg.len) {
                written_map_set(&written_map, ms, &seg);
            }
            free(seg.data);
            if (written != (size_t)seg.len) {
                /* disk full or the reader of stdout went away */
                MSG_ERROR("Could not write segment %d.\n", ms->sequence_number);
                download = false;
                result = 1;
                pthread_cancel(thread);
                break;
            }

            set_fresh_connect_http_session(session, 0);

            time_t curRepTime = time(NULL);
            if ((curRepTime - repTime) >= 1) {
                MSG_API("{\"t_d\":%u,\"d_d\":%u,\"d_s\":%"PRId64"}\n", (uint32_t)(me->total_duration_ms / 1000), (uint32_t)(downloaded_duration_ms / 1000), download_size);
                repTime = curRepTime;
            }

            break;
        } while(true);

loop_cleanup:
        media_segment_cleanup(ms);
    }

    written_map_free(&written_map);

    if (audio_thread_running) {
        stop_updater_thread(&audio_params, audio_thread);
    }
    if (me_audio) {
        /* what the audio refresh queued after the last video segment */
        while (me_audio->first_media_segment) {
            struct hls_media_segment *as = me_audio->first_media_segment;
            unlink_queue_head(me_audio);
            media_segment_cleanup(as);
        }
    }
    if (audio_session) {
        clean_http_session(audio_session);
    }

    pthread_join(thread, &ret);
    pthread_mutex_destroy(&media_playlist_mtx);

    pthread_cond_destroy(&media_playlist_refresh_cond);
    pthread_cond_destroy(&media_playlist_empty_cond);

    pthread_mutex_destroy(&cookie_file_mtx);
    hls_args.cookie_file_mutex = NULL;

    MSG_API("{\"t_d\":%u,\"d_d\":%u,\"d_s\":%"PRId64"}\n", (uint32_t)(me->total_duration_ms / 1000), (uint32_t)(downloaded_duration_ms / 1000), download_size);
    clean_http_session(session);

    return result;
}

/* vod_download_segment() results besides 0: VOD_SEG_DOWNLOAD_ERROR is a
 * transfer that still failed after the retries - the only kind -I skips; key,
 * decryption and output errors stay fatal. */
#define VOD_SEG_ERROR          1
#define VOD_SEG_DOWNLOAD_ERROR 2

static int vod_download_segment(void **psession, hls_media_playlist_t *me, struct hls_media_segment *ms, struct ByteBuffer *seg)
{
    int retries = 0;
    int ret = 0;
    while (true) {
        if (ms->is_map) {
            MSG_PRINT("Downloading init segment %s\n", ms->url);
        } else {
            MSG_PRINT("Downloading part %d\n", ms->sequence_number);
        }

        memset(seg, 0x00, sizeof(*seg));
        size_t size = 0;
        long http_code = get_data_from_url_with_session(psession, ms->url, (char **)&(seg->data), &size, BINARY, NULL, ms->offset, ms->size);
        seg->len = (int)size;
        bool http_ok = http_code == 200 || (http_code == 206 && (ms->size > -1 || hls_args.accept_partial_content));
        /* an empty body counts as a failed transfer (retried, skipped by -I) */
        if (!http_ok || size == 0) {
            if (seg->data) {
                free(seg->data);
                seg->data = NULL;
            }
            /* -w N = at most N retries (-w 0: none) */
            if (http_code != 403 && http_code != 401 && http_code != 410 && retries < hls_args.segment_download_retries) {
                clean_http_session(*psession);
                sleep(1);
                *psession = init_hls_session();
                set_timeout_session(*psession, 2L, 30L);
                set_fresh_connect_http_session(*psession, 1);
                MSG_WARNING("VOD retry segment %d download, due to previous error. http_code[%d].\n", ms->sequence_number, (int)http_code);
                retries += 1;
                continue;
            }
            ret = VOD_SEG_DOWNLOAD_ERROR;
            if (http_ok) {
                MSG_ERROR("Segment %d came back empty.\n", ms->sequence_number);
                MSG_API("{\"error_code\":-1, \"error_msg\":\"empty\"}\n");
            } else {
                MSG_API("{\"error_code\":%d, \"error_msg\":\"http\"}\n", (int)http_code);
            }
            break;
        }
        if (unusable_segment_response(ms, http_code, size)) {
            MSG_ERROR("Segment %s: the response does not match its byte range.\n", ms->url);
            MSG_API("{\"error_code\":-1, \"error_msg\":\"range\"}\n");
            ret = VOD_SEG_ERROR;
        }
        break;
    }

    /* A segment that cannot be decrypted fails the VOD download instead of
     * landing in the file as garbage; with -R the download can carry on from
     * here later. For SAMPLE-AES only a missing key or no memory (return 1)
     * is fatal - its other errors (no PMT, unknown streams) still write the
     * segment. */
    if (ret == 0) {
        bool decrypt_failed = ms->encryptiontype == ENC_AES128 && 0 != decrypt_aes128(ms, seg);
        if (!decrypt_failed && !ms->is_map) {
            strip_disguise_prefix(seg);
        }
        if (!decrypt_failed && ms->encryptiontype == ENC_AES_SAMPLE) {
            decrypt_failed = 1 == decrypt_sample_aes(ms, seg);
        }
        if (decrypt_failed) {
            MSG_ERROR("Could not decrypt segment %d.\n", ms->sequence_number);
            MSG_API("{\"error_code\":-1, \"error_msg\":\"decrypt\"}\n");
            ret = VOD_SEG_ERROR;
        }
    }
    if (ret != 0) {
        free(seg->data);
        seg->data = NULL;
    }

    /* normally we want to reuse sessions,
     * so restore it in case when fresh session
     * was requested do to re-try
     */
    if (retries) {
        set_fresh_connect_http_session(*psession, 0);
    }

    return ret;
}

bool consecutive_sync_byte(uint8_t *buf, size_t len, uint8_t n) {
    if (len < n * TS_PACKET_LENGTH) {
        return false;
    }

    for (uint8_t i = 1; i < n; ++i) {
        if (buf[i * TS_PACKET_LENGTH] != TS_SYNC_BYTE) {
            return false;
        }
    }

    return true;
}

uint8_t * find_first_ts_packet(ByteBuffer_t *buf) {
    uint8_t *cursor = buf->data;
    size_t len = buf->len;

    while (TS_PACKET_LENGTH <= len) {
        uint8_t *next = memchr(cursor, TS_SYNC_BYTE, len);
        if (next == NULL) {
            return NULL;
        }
        len -= (next - cursor);
        cursor = next;
        if (consecutive_sync_byte(cursor, len, 3)) {
            return cursor;
        }
        ++cursor;
        --len;
    }

    return NULL;
}

/* Write one VOD segment. false when the output took fewer bytes (disk full) -
 * the download then fails instead of reporting an incomplete file as done; the
 * resume sidecar still points before this segment. */
static bool vod_write(write_ctx_t *out_ctx, const uint8_t *data, int len, int64_t *download_size)
{
    size_t written = out_ctx->write(data, (size_t)len, out_ctx->opaque);
    *download_size += written;
    if (written != (size_t)len) {
        MSG_ERROR("Could not write to the output file.\n");
        MSG_API("{\"error_code\":-1, \"error_msg\":\"write\"}\n");
        return false;
    }
    return true;
}

/* Persist resume progress after a media segment. Flush the output, then take
 * the byte count from the real file position rather than a running counter -
 * that way a miscount anywhere in the writer (e.g. a short write counted in
 * full) cannot desync the sidecar from the file. */
static void resume_checkpoint(hls_resume_state_t *resume, write_ctx_t *out_ctx, int done, int64_t bytes)
{
    if (!resume) {
        return;
    }
    if (out_ctx && out_ctx->opaque) {
        FILE *f = (FILE *)out_ctx->opaque;
        fflush(f);
#ifdef _MSC_VER
        __int64 pos = _ftelli64(f);
#else
        off_t pos = ftello(f);
#endif
        if (pos >= 0) {
            bytes = (int64_t)pos;
        }
    }
    resume_save(resume, done, bytes);
}

int download_hls(write_ctx_t *out_ctx, hls_media_playlist_t *me, hls_media_playlist_t *me_audio, hls_resume_state_t *resume)
{
    MSG_VERBOSE("Downloading segments.\n");
    MSG_API("{\"d_t\":\"vod\"}\n"); // d_t - download type
    MSG_API("{\"t_d\":%u,\"d_d\":0, \"d_s\":0}\n", (uint32_t)(me->total_duration_ms / 1000)); // t_d - total duration, d_d  - download duration, d_s - download size

    int ret = 0;
    void *session = init_hls_session();
    set_timeout_session(session, 2L, 3L);
    time_t repTime = 0;

    int total_media_segments = 0;
    for (struct hls_media_segment *t = me->first_media_segment; t; t = t->next) {
        if (!t->is_map) {
            total_media_segments++;
        }
    }

    uint64_t downloaded_duration_ms = 0;
    int media_seg_done = resume ? resume->done : 0;
    int64_t download_size = resume ? resume->bytes : 0;
    struct ByteBuffer seg;
    struct ByteBuffer seg_audio;

    struct hls_media_segment *ms = me->first_media_segment;
    struct hls_media_segment *ms_audio = NULL;
    merge_context_t merge_context;
    bool drop_audio = false;
    bool audio_map_written = (resume && resume->done > 0);
    int skipped_segments = 0;   /* -I */
    int written_segments = 0;
    written_map_t written_map = {0};
    /* a discontinuity was crossed since the last A/V merge - kept until the
     * next merge, also when -I skips the flagged segment */
    bool merge_reset_pending = false;

    if (me_audio) {
        ms_audio = me_audio->first_media_segment;
        memset(&merge_context, 0x00, sizeof(merge_context));
        merge_context.out = out_ctx;
    }

    if (resume && resume->done >= total_media_segments && total_media_segments > 0) {
        MSG_PRINT("Nothing to resume - the download was already complete.\n");
        /* a final progress line, so a frontend that only reads the JSON status
         * sees the finished size instead of the initial "d_s":0 */
        MSG_API("{\"t_d\":%u,\"d_d\":%u,\"d_s\":%"PRId64"}\n", (uint32_t)(me->total_duration_ms / 1000), (uint32_t)(me->total_duration_ms / 1000), resume->bytes);
        resume_clear(resume->out_filename);
        clean_http_session(session);
        return 0;
    }

    /* Fast-forward past the media segments a previous run already wrote,
     * without fetching or writing anything. The video cursor is the one that
     * matters; the TS+merge path keeps the audio cursor in lockstep with it
     * (as the main loop does), and the fMP4 path does not use the audio
     * cursor at all. */
    if (resume && resume->done > 0) {
        int skipped = 0;
        while (ms && skipped < resume->done) {
            if (ms->is_map) {
                /* already in the file - a repeat of it after the resume point
                 * must not be written again */
                written_map_set(&written_map, ms, NULL);
                ms = ms->next;
                continue;
            }
            if (me_audio && ms_audio && ms_audio->is_map) {
                ms_audio = ms_audio->next;
                continue;
            }
            downloaded_duration_ms += ms->duration_ms;   /* keep the progress report honest */
            ms = ms->next;
            if (me_audio && ms_audio) {
                ms_audio = ms_audio->next;
            }
            skipped++;
        }
        MSG_VERBOSE("Resumed past %d segments.\n", resume->done);
    }

    while(ms) {
        /* Initialization segment (EXT-X-MAP): write it verbatim, never run it
         * through the TS packet scanner or the audio/video merge, and do not
         * consume a segment from the other playlist for it. */
        if (ms->is_map) {
            /* a repeated EXT-X-MAP (same source) is not fetched again */
            if (written_map_same_source(&written_map, ms)) {
                ms = ms->next;
                continue;
            }
            if (0 != vod_download_segment(&session, me, ms, &seg)) {
                ret = 1;
                break;
            }
            if (0 != detect_media_type(me, &seg)) {
                free(seg.data);
                ret = 1;
                break;
            }
            /* same init bytes under another URL: nothing to write */
            bool map_ok = written_map_same_bytes(&written_map, &seg)
                       || vod_write(out_ctx, seg.data, seg.len, &download_size);
            if (map_ok) {
                written_map_set(&written_map, ms, &seg);
            }
            free(seg.data);
            if (!map_ok) {
                ret = 1;
                break;
            }
            ms = ms->next;
            continue;
        }
        if (ms_audio && ms_audio->is_map) {
            if (0 != vod_download_segment(&session, me_audio, ms_audio, &seg_audio)) {
                ret = 1;
                break;
            }
            /* A separate audio rendition cannot be muxed into fMP4 output;
             * drop its init header. me->media_type is normally known here (the
             * video EXT-X-MAP is the first list entry), but the guard also
             * copes with an audio init that somehow arrives first. */
            if (!drop_audio && me->media_type == MEDIA_TYPE_FMP4) {
                MSG_WARNING("Separate audio track with fragmented MP4 - hlsdl writes the video track only; use a remuxer for muxed output.\n");
                drop_audio = true;
            }
            /* Only the leading audio init segment may be written raw. A
             * second one mid-stream (re-init after a discontinuity) would
             * land inside the muxed TS and corrupt it. */
            if (!drop_audio && !audio_map_written) {
                if (!vod_write(out_ctx, seg_audio.data, seg_audio.len, &download_size)) {
                    free(seg_audio.data);
                    ret = 1;
                    break;
                }
                audio_map_written = true;
            } else if (!drop_audio) {
                MSG_WARNING("Mid-stream audio EXT-X-MAP ignored - re-init after a discontinuity is not handled.\n");
            }
            free(seg_audio.data);
            ms_audio = ms_audio->next;
            continue;
        }

        if (ms->discontinuity || (ms_audio && ms_audio->discontinuity)) {
            merge_reset_pending = true;
        }
        int seg_ret = vod_download_segment(&session, me, ms, &seg);
        if (0 != seg_ret) {
            if (seg_ret == VOD_SEG_DOWNLOAD_ERROR && hls_args.ignore_download_errors) {
                /* -I: leave a gap; the paired audio segment goes with it so
                 * the TS merge stays in step (fMP4 does not use that cursor) */
                MSG_WARNING("Skipping segment %d - the output will have a gap.\n", ms->sequence_number);
                skipped_segments++;
                ms = ms->next;
                if (ms_audio && me->media_type != MEDIA_TYPE_FMP4) {
                    ms_audio = ms_audio->next;
                }
                continue;
            }
            ret = 1;
            break;
        }

        if (0 != detect_media_type(me, &seg)) {
            free(seg.data);
            ret = 1;
            break;
        }

        if (ms->discontinuity) {
            MSG_WARNING("Crossing EXT-X-DISCONTINUITY at segment %d - output may not be seamless.\n", ms->sequence_number);
        }

        /* Fragmented MP4: concatenate init + media fragments verbatim. A
         * separate audio rendition cannot be muxed here. */
        if (me->media_type == MEDIA_TYPE_FMP4) {
            if (ms_audio && !drop_audio) {
                MSG_WARNING("Separate audio track with fragmented MP4 - hlsdl writes the video track only; use a remuxer for muxed output.\n");
                drop_audio = true;
            }
            bool frag_ok = vod_write(out_ctx, seg.data, seg.len, &download_size);
            free(seg.data);
            if (!frag_ok) {
                ret = 1;
                break;
            }
            written_segments++;
            downloaded_duration_ms += ms->duration_ms;
            time_t curRepTime = time(NULL);
            if ((curRepTime - repTime) >= 1) {
                MSG_API("{\"t_d\":%u,\"d_d\":%u,\"d_s\":%"PRId64"}\n", (uint32_t)(me->total_duration_ms / 1000), (uint32_t)(downloaded_duration_ms / 1000), download_size);
                repTime = curRepTime;
            }
            ms = ms->next;
            media_seg_done++;
            resume_checkpoint(resume, out_ctx, media_seg_done, download_size);
            continue;
        }

        uint8_t *first_video_packet = find_first_ts_packet(&seg);
        uint8_t *first_audio_packet = NULL;
        if (ms_audio) {
            int audio_ret = vod_download_segment(&session, me_audio, ms_audio, &seg_audio);
            if (0 != audio_ret) {
                free(seg.data);
                if (audio_ret == VOD_SEG_DOWNLOAD_ERROR && hls_args.ignore_download_errors) {
                    /* -I: drop the video segment too, so both tracks have
                     * the same gap */
                    MSG_WARNING("Skipping segment %d - the output will have a gap.\n", ms->sequence_number);
                    skipped_segments++;
                    ms = ms->next;
                    ms_audio = ms_audio->next;
                    continue;
                }
                ret = 1;
                break;
            }
            first_audio_packet = audio_merge_start(&seg_audio);
        }

        // first segment should be TS for success merge
        if (first_video_packet && first_audio_packet) {
            size_t video_len = seg.len - (first_video_packet - seg.data);
            size_t audio_len = seg_audio.len - (first_audio_packet - seg_audio.data);

            /* the PIDs / PMT may have changed at the discontinuity: the merge
             * works its PMT out again */
            if (merge_reset_pending) {
                merge_context_reset(&merge_context);
                merge_reset_pending = false;
            }
            size_t merged = merge_packets(
                &merge_context,
                first_video_packet,
                video_len,
                first_audio_packet,
                audio_len
            );
            download_size += merged;
            if (!merged && first_audio_packet == seg_audio.data && !memcmp(seg_audio.data, "ID3", 3)) {
                /* packed audio hlsdl cannot pack (no timestamp in the ID3
                 * tag, unknown codec): the video goes in alone, as it always
                 * did before packed audio was merged */
                MSG_WARNING("Could not merge the packed audio of segment %d - writing the video only.\n", ms->sequence_number);
                if (!vod_write(out_ctx, seg.data, seg.len, &download_size)) {
                    ret = 1;
                }
            } else if (!merged) {
                /* 0 = no PMT to merge or nothing could be written - the
                 * segment would silently be missing from the file */
                MSG_ERROR("Could not merge audio and video of segment %d.\n", ms->sequence_number);
                MSG_API("{\"error_code\":-1, \"error_msg\":\"merge\"}\n");
                ret = 1;
            }
        } else if (!vod_write(out_ctx, seg.data, seg.len, &download_size)) {
            ret = 1;
        }

        if (ms_audio) {
            free(seg_audio.data);
            ms_audio = ms_audio->next;
        }

        free(seg.data);
        if (ret) {
            break;
        }
        written_segments++;

        downloaded_duration_ms += ms->duration_ms;

        time_t curRepTime = time(NULL);
        if ((curRepTime - repTime) >= 1) {
            MSG_API("{\"t_d\":%u,\"d_d\":%u,\"d_s\":%"PRId64"}\n", (uint32_t)(me->total_duration_ms / 1000), (uint32_t)(downloaded_duration_ms / 1000), download_size);
            repTime = curRepTime;
        }

        ms = ms->next;
        media_seg_done++;
        resume_checkpoint(resume, out_ctx, media_seg_done, download_size);
    }

    if (ret == 0 && skipped_segments > 0) {
        if (written_segments == 0) {
            /* -I must not turn "nothing could be fetched" into a success */
            MSG_ERROR("No media segment could be downloaded.\n");
            ret = 1;
        } else {
            MSG_WARNING("%d of %d segments were skipped - the output has gaps.\n", skipped_segments, total_media_segments);
        }
    }

    if (resume && ret == 0) {
        resume_clear(resume->out_filename);
    }

    MSG_API("{\"t_d\":%u,\"d_d\":%u,\"d_s\":%"PRId64"}\n", (uint32_t)(me->total_duration_ms / 1000), (uint32_t)(downloaded_duration_ms / 1000), download_size);

    written_map_free(&written_map);
    clean_http_session(session);

    return ret;
}

int print_enc_keys(hls_media_playlist_t *me)
{
    struct hls_media_segment *ms = me->first_media_segment;
    while(ms) {
        if (ms->encryptiontype != ENC_NONE) {
            fill_key_value(&(ms->enc_aes));
            MSG_PRINT("[AES-128]KEY: 0x");
            for(size_t count = 0; count < KEYLEN; count++) {
                MSG_PRINT("%02x", ms->enc_aes.key_value[count]);
            }
            MSG_PRINT(" IV: 0x");
            for(size_t count = 0; count < KEYLEN; count++) {
                MSG_PRINT("%02x", ms->enc_aes.iv_value[count]);
            }
            MSG_PRINT("\n");
        }
        ms = ms->next;
    }
    return 0;
}

void media_segment_cleanup(struct hls_media_segment *ms)
{
    if (ms)
    {
        free(ms->url);
        free(ms->enc_aes.key_url);
        free(ms);
    }
}

void media_playlist_cleanup(hls_media_playlist_t *me)
{
    struct hls_media_segment *ms = me->first_media_segment;
    free(me->source);
    free(me->orig_url);
    free(me->url);
    free(me->audio_grp);
    free(me->resolution);
    free(me->codecs);
    free(me->enc_aes.key_url);

    while(ms){
        me->first_media_segment = ms->next;
        media_segment_cleanup(ms);
        ms = me->first_media_segment;
    }
    assert(me->first_media_segment == NULL);
    me->last_media_segment = NULL;
}

static void audio_cleanup(hls_audio_t *audio)
{
    free(audio->url);
    free(audio->grp_id);
    free(audio->lang);
    free(audio->name);
}

void master_playlist_cleanup(struct hls_master_playlist *ma)
{
    hls_media_playlist_t *me = ma->media_playlist;
    while (me) {
        hls_media_playlist_t *ptr = me;
        me = me->next;
        media_playlist_cleanup(ptr);
        free(ptr);
    }

    hls_audio_t *audio = ma->audio;
    while (audio) {
        hls_audio_t *ptr = audio;
        audio = audio->next;
        audio_cleanup(ptr);
        free(ptr);
    }

    free(ma->source);
    free(ma->orig_url);
    free(ma->url);
}

/* Keys fetched so far, by key URL. Several entries, because a separate audio
 * rendition usually has a key of its own: with a single entry the video and
 * the audio key pushed each other out and every segment cost a key request.
 * Replaced round robin. Used from the downloading thread only. */
#define KEY_CACHE_SIZE 8

static struct {
    char *url;
    uint8_t value[KEYLEN];
} key_cache[KEY_CACHE_SIZE];
static int key_cache_next = 0;
/* kept between key requests, so a key costs no new TCP / TLS handshake */
static void *key_session = NULL;

static void key_cache_put(const char *url, const uint8_t *value)
{
    char *copy = strdup(url);
    if (!copy) {
        return;
    }
    free(key_cache[key_cache_next].url);
    key_cache[key_cache_next].url = copy;
    memcpy(key_cache[key_cache_next].value, value, KEYLEN);
    key_cache_next = (key_cache_next + 1) % KEY_CACHE_SIZE;
}

static bool key_cache_get(const char *url, uint8_t *value)
{
    for (int i = 0; i < KEY_CACHE_SIZE; i++) {
        if (key_cache[i].url && 0 == strcmp(key_cache[i].url, url)) {
            memcpy(value, key_cache[i].value, KEYLEN);
            return true;
        }
    }
    return false;
}

void fill_key_value_cleanup(void)
{
    for (int i = 0; i < KEY_CACHE_SIZE; i++) {
        free(key_cache[i].url);
        key_cache[i].url = NULL;
    }
    if (key_session) {
        clean_http_session(key_session);
        key_session = NULL;
    }
}

int fill_key_value(struct enc_aes128 *es)
{
    if (es && es->key_url)
    {
        if (key_cache_get(es->key_url, es->key_value))
        {
            /* known key */
        }
        else if (hls_args.key_value)
        {
            memcpy(es->key_value, hls_args.key_value, KEYLEN);
            key_cache_put(es->key_url, es->key_value);
        } else
        {
            char *key_url = NULL;
            char *key_value = NULL;
            size_t size = 0;
            long http_code = 0;

            if (NULL != hls_args.key_uri_replace_old && \
                NULL != hls_args.key_uri_replace_new && \
                '\0' != hls_args.key_uri_replace_old[0]) {
                key_url = repl_str(es->key_url, hls_args.key_uri_replace_old, hls_args.key_uri_replace_new);
            } else {
                key_url = es->key_url;
            }

            if (!key_session) {
                key_session = init_hls_session();
                set_timeout_session(key_session, 2L, 15L);
            }
            http_code = get_data_from_url_with_session(&key_session, key_url, &key_value, &size, BINKEY, NULL, -1, -1);
            if (es->key_url != key_url) {
                free(key_url);
            }

            if (http_code != 200) {
                MSG_ERROR("Getting key-file [%s] failed http_code[%d].\n", es->key_url, (int)http_code);
                free(key_value);
                /* the next attempt starts on a fresh connection */
                clean_http_session(key_session);
                key_session = NULL;
                return 1;
            }

            if (size != KEYLEN || !key_value) {
                MSG_ERROR("Wrong length key-file. Expected %u bytes but got %u.\n", KEYLEN, (unsigned)size);
                free(key_value);
                return 1;
            }

            memcpy(es->key_value, key_value, KEYLEN);
            free(key_value);

            key_cache_put(es->key_url, es->key_value);
        }

        free(es->key_url);
        es->key_url = NULL;
    }

    return 0;
}
