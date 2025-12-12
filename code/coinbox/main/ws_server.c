/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "ws_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "logger.h"
#include "files.h"
#include "mount.h"

#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "esp_http_server.h"

#include "ota.h"
#include <ctype.h>

/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_LITTLEFS_OBJ_NAME_LEN)

/* Max size of an individual file. Make sure this
 * value is same as that set in fileserver.html */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

static const char *ALLOWED_AUDIO_EXTS[] = { "mp3", "wav", "ogg", "flac", "aac", "m4a" };
static const size_t ALLOWED_AUDIO_EXTS_COUNT = sizeof(ALLOWED_AUDIO_EXTS) / sizeof(ALLOWED_AUDIO_EXTS[0]);
#define ALLOWED_AUDIO_EXTS_LIST ".mp3, .wav, .ogg, .flac, .aac, .m4a"

struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

static void url_decode_inplace(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst = (char) strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst = ' ';
            src++;
        } else {
            *dst = *src;
            src++;
        }
        dst++;
    }
    *dst = '\0';
}

/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    return ESP_OK;
}

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static esp_err_t http_resp_index_html(httpd_req_t *req)
{
    extern const unsigned char indfex_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - indfex_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)indfex_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t http_resp_mt_html(httpd_req_t *req)
{
    extern const unsigned char mt_html_start[] asm("_binary_mt_html_start");
    extern const unsigned char mt_html_end[] asm("_binary_mt_html_end");
    const size_t mt_html_size = (mt_html_end - mt_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)mt_html_start, mt_html_size);
    return ESP_OK;
}

static esp_err_t http_resp_logs(httpd_req_t *req)
{
    const char *buf = logger_get_buffer();
    size_t len = logger_get_size();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t http_resp_navbar_js(httpd_req_t *req)
{
    extern const unsigned char navbar_js_start[] asm("_binary_navbar_js_start");
    extern const unsigned char navbar_js_end[] asm("_binary_navbar_js_end");
    const size_t navbar_js_size = (navbar_js_end - navbar_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)navbar_js_start, navbar_js_size);
    return ESP_OK;
}

static esp_err_t format_storage_handler(httpd_req_t *req)
{
    struct file_server_data *data = (struct file_server_data *)req->user_ctx;
    if (!data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No context");
        return ESP_FAIL;
    }
    if (format_storage(data->base_path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format failed");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, "Formatted");
    return ESP_OK;
}

/* Send HTTP response with a run-time generated html consisting of
 * a list of all files and folders under the requested path.
 * In case of SPIFFS this returns empty list when path is any
 * string other than '/', since SPIFFS doesn't support directories */
static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
{
    char entrypath[FILE_PATH_MAX];
    const char *entrytype;

    struct dirent *entry;
    struct stat entry_stat;

    DIR *dir = opendir(dirpath);
    const size_t dirpath_len = strlen(dirpath);
    (void)dirpath_len;

    /* Retrieve the base path of file storage to construct the full path */
    strlcpy(entrypath, dirpath, sizeof(entrypath));

    if (!dir) {
        logger_loge(TAG, "Failed to stat dir : %s", dirpath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
        return ESP_FAIL;
    }

    /* Get handle to embedded file upload script */
    extern const unsigned char upload_script_start[] asm("_binary_fileserver_html_start");
    extern const unsigned char upload_script_end[]   asm("_binary_fileserver_html_end");
    const size_t upload_script_size = (upload_script_end - upload_script_start);

    /* Base page and drop area */
    httpd_resp_send_chunk(req, (const char *)upload_script_start, upload_script_size);
    /* Expose current path for client-side uploads */
    httpd_resp_sendstr_chunk(req, "<script>window.CURRENT_PATH='");
    httpd_resp_sendstr_chunk(req, req->uri);
    if (req->uri[strlen(req->uri) - 1] != '/') {
        httpd_resp_sendstr_chunk(req, "/");
    }
    httpd_resp_sendstr_chunk(req, "';</script>");

    int row_idx = 0;
    /* Iterate over all files / folders and fetch their names and sizes */
    while ((entry = readdir(dir)) != NULL) {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

        int len = snprintf(entrypath, sizeof(entrypath), "%s/%s",
                       dirpath, entry->d_name);
        if (len < 0 || len >= sizeof(entrypath)) {
            logger_loge(TAG, "Path too long: %s + %s", dirpath, entry->d_name);
            continue;
        }
        if (stat(entrypath, &entry_stat) == -1) {
            logger_loge(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
            continue;
        }
        logger_logi(TAG, "Found %s : %s", entrytype, entry->d_name);

        bool is_dir = (entry->d_type == DT_DIR);
        if (is_dir) {
            httpd_resp_sendstr_chunk(req, "<div class=\"file-item\"><div class=\"file-row-main\">");
            httpd_resp_sendstr_chunk(req, "<a class=\"file-name\" href=\"");
            httpd_resp_sendstr_chunk(req, req->uri);
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "/\" title=\"");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "\">");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "</a></div></div>\n");
            continue;
        }

        file_entry_t meta;
        if (files_read_header(entry->d_name, &meta) != ESP_OK) {
            logger_logw(TAG, "Skipping non-entry file: %s", entrypath);
            continue;
        }

        char row_id[32];
        snprintf(row_id, sizeof(row_id), "row-%d", row_idx++);

        httpd_resp_sendstr_chunk(req, "<div class=\"file-item\" id=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\" data-file-uri=\"");
        httpd_resp_sendstr_chunk(req, req->uri);
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\" data-probability=\"");
        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)meta.props.probability);
        httpd_resp_sendstr_chunk(req, numbuf);
        httpd_resp_sendstr_chunk(req, "\" data-volume=\"");
        snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)meta.props.volume);
        httpd_resp_sendstr_chunk(req, numbuf);
        httpd_resp_sendstr_chunk(req, "\" data-enabled=\"");
        httpd_resp_sendstr_chunk(req, meta.props.enabled ? "1" : "0");
        httpd_resp_sendstr_chunk(req, "\">");

        httpd_resp_sendstr_chunk(req, "<div class=\"file-row-main\">");
        httpd_resp_sendstr_chunk(req, "<div class=\"file-name-wrap\"><a class=\"file-name\" href=\"");
        httpd_resp_sendstr_chunk(req, req->uri);
        httpd_resp_sendstr_chunk(req, entry->d_name);
        logger_logi(TAG, "Request URI: %s, Entry Name: %s", req->uri, entry->d_name);
        logger_logi(TAG, "Incoming dirpath: %s", dirpath);
        httpd_resp_sendstr_chunk(req, "\" title=\"");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "</a>");
        httpd_resp_sendstr_chunk(req, "<input class=\"file-name-edit\" data-k=\"name-edit\" type=\"text\" autocomplete=\"off\" spellcheck=\"false\">");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<div class=\"row-actions\">");
        httpd_resp_sendstr_chunk(req, "<label class=\"switch\"><input type=\"checkbox\" data-k=\"enabled-toggle\"><span class=\"slider\"></span></label>");
        httpd_resp_sendstr_chunk(req, "<button class=\"icon-btn play-btn\" data-k=\"play\" aria-label=\"Play\"><svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M8.5 5.5v13l9-6.5-9-6.5Z\"/></svg></button>");
        httpd_resp_sendstr_chunk(req, "<button class=\"chev\" data-row-id=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\" data-open=\"0\">&#9881;</button>");
        httpd_resp_sendstr_chunk(req, "</div></div>");

        httpd_resp_sendstr_chunk(req, "<div class=\"details\" data-for-row=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, "<div class=\"details-inner\">");

        httpd_resp_sendstr_chunk(req, "<div class=\"prop prob-row\"><label>Probability</label>");
        httpd_resp_sendstr_chunk(req, "<div class=\"slider-wrap\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" data-k=\"probability-minus\">-</button>");
        httpd_resp_sendstr_chunk(req, "<input type=\"range\" min=\"0\" max=\"100\" data-k=\"probability\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" data-k=\"probability-plus\">+</button>");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<input type=\"number\" min=\"0\" max=\"100\" data-k=\"probability-num\">");
        httpd_resp_sendstr_chunk(req, "<span class=\"percent\">%</span>");
        httpd_resp_sendstr_chunk(req, "</div>");

        httpd_resp_sendstr_chunk(req, "<div class=\"prop vol-row\"><label>Volume</label>");
        httpd_resp_sendstr_chunk(req, "<div class=\"slider-wrap\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" data-k=\"volume-minus\">-</button>");
        httpd_resp_sendstr_chunk(req, "<input type=\"range\" min=\"0\" max=\"100\" data-k=\"volume\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" data-k=\"volume-plus\">+</button>");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<input type=\"number\" min=\"0\" max=\"100\" data-k=\"volume-num\">");
        httpd_resp_sendstr_chunk(req, "<span class=\"percent\">%</span>");
        httpd_resp_sendstr_chunk(req, "</div>");

        httpd_resp_sendstr_chunk(req, "<div class=\"prop action-row\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"delete-btn\" data-row-id=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\"><svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M9 3a1 1 0 0 0-1 1v1H5.5a1 1 0 1 0 0 2H6v12a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V7h0.5a1 1 0 1 0 0-2H16V4a1 1 0 0 0-1-1H9Zm1 2h4V5h-4V5Zm-1 4a1 1 0 1 1 2 0v8a1 1 0 1 1-2 0V9Zm6-1a1 1 0 0 1 1 1v8a1 1 0 1 1-2 0V9a1 1 0 0 1 1-1Z\"/></svg>Delete file</button>");
        httpd_resp_sendstr_chunk(req, "</div>");

        httpd_resp_sendstr_chunk(req, "</div></div>");
        httpd_resp_sendstr_chunk(req, "</div>\n");
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "</div></div></body></html>");

    /* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".mp3")) {
        return httpd_resp_set_type(req, "audio/mpeg");
    } else if (IS_FILE_EXT(filename, ".wav")) {
        return httpd_resp_set_type(req, "audio/wav");
    } else if (IS_FILE_EXT(filename, ".ogg")) {
        return httpd_resp_set_type(req, "audio/ogg");
    } else if (IS_FILE_EXT(filename, ".flac")) {
        return httpd_resp_set_type(req, "audio/flac");
    } else if (IS_FILE_EXT(filename, ".aac")) {
        return httpd_resp_set_type(req, "audio/aac");
    } else if (IS_FILE_EXT(filename, ".m4a")) {
        return httpd_resp_set_type(req, "audio/mp4");
    } else if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

static bool is_audio_filename(const char *name)
{
    if (!name || !*name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || *(dot + 1) == '\0') {
        return false;
    }
    const char *ext = dot + 1;
    for (size_t i = 0; i < ALLOWED_AUDIO_EXTS_COUNT; ++i) {
        if (strcasecmp(ext, ALLOWED_AUDIO_EXTS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    const char *result = dest + base_pathlen;
    logger_logi(TAG, "Destination: %s, Base Path: %s, Return: %s", dest, base_path, result);
    return result;

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;
    bool delete_requested = false;
    bool rename_requested = false;
    char rename_val[128] = {0};

    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/file-server") - 1, sizeof(filepath));
    if (!filename) {
        logger_loge(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }
    size_t filename_len = strlen(filename);
    char filename_last_char = filename_len ? filename[filename_len - 1] : '\0';
    logger_logi(TAG, "Fileserver found : %s, filename: %s, filenamestrlen: %c", filepath, filename,
             filename_last_char ? filename_last_char : ' ');

    if (filename_len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Trailing slash required");
        return ESP_FAIL;
    }

    const char *base_name = filename;
    const char *slash_in_filename = strrchr(filename, '/');
    if (slash_in_filename && *(slash_in_filename + 1)) {
        base_name = slash_in_filename + 1;
    }

    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len) {
        char *query_str = calloc(1, query_len + 1);
        if (!query_str) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, query_str, query_len + 1) == ESP_OK) {
            char delete_val[2];
            if (httpd_query_key_value(query_str, "delete", delete_val, sizeof(delete_val)) == ESP_OK) {
                delete_requested = true;
            }
            if (httpd_query_key_value(query_str, "rename", rename_val, sizeof(rename_val)) == ESP_OK) {
                url_decode_inplace(rename_val);
                rename_requested = true;
            }
        }
        free(query_str);
    }

    if (filename_last_char == '/') {
        return http_resp_dir_html(req, filepath);
    }
    if (rename_requested && rename_val[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rename target missing");
        return ESP_FAIL;
    }
    if (rename_requested && (strchr(rename_val, '/') || strchr(rename_val, '\\'))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rename");
        return ESP_FAIL;
    }
    
    if (delete_requested) {
        if (stat(filepath, &file_stat) == -1) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
            return ESP_FAIL;
        }
        if (unlink(filepath) != 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
            return ESP_FAIL;
        }
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/file-server/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
        httpd_resp_set_hdr(req, "Connection", "close");
#endif
        httpd_resp_sendstr(req, "File deleted successfully");
        return ESP_OK;
    }

    if (rename_requested) {
        if (stat(filepath, &file_stat) == -1) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
            return ESP_FAIL;
        }
        char *last_slash = strrchr(filepath, '/');
        if (!last_slash) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad path");
            return ESP_FAIL;
        }
        size_t dir_len = (size_t)(last_slash - filepath + 1);
        char new_filepath[FILE_PATH_MAX];
        if (dir_len + strlen(rename_val) >= sizeof(new_filepath)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "New name too long");
            return ESP_FAIL;
        }
        memcpy(new_filepath, filepath, dir_len);
        strlcpy(new_filepath + dir_len, rename_val, sizeof(new_filepath) - dir_len);
        if (stat(new_filepath, &file_stat) == 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "Target exists");
            return ESP_OK;
        }
        if (rename(filepath, new_filepath) != 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
            return ESP_FAIL;
        }
        httpd_resp_sendstr(req, "Renamed");
        return ESP_OK;
    }

    if (stat(filepath, &file_stat) == -1) {
        /* If file not present on SPIFFS check if URI
         * corresponds to one of the hardcoded paths */
        if (strcmp(filename, "/index.html") == 0) {
            return index_html_get_handler(req);
        } else if (strcmp(filename, "/favicon.ico") == 0) {
            return favicon_get_handler(req);
        } else if (strcmp(filename, "/navbar.js") == 0) {
            return http_resp_navbar_js(req);
        } else if (strcmp(filename, "/mt") == 0) {
            return http_resp_mt_html(req);
        }
        logger_loge(TAG, "Failed to stat file : l%sl", filepath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd) {
        logger_loge(TAG, "Failed to read existing file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    file_entry_t meta;
    if (files_read_header(base_name, &meta) != ESP_OK) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File entry missing");
        return ESP_FAIL;
    }

    if (fseek(fd, (long)FILE_HEADER_SIZE, SEEK_SET) != 0) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to seek");
        return ESP_FAIL;
    }

    logger_logi(TAG, "Sending file : %s (%zu bytes)...", filename, meta.data_size);
    set_content_type_from_file(req, filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    size_t remaining = meta.data_size;
    while (remaining > 0) {
        size_t to_read = remaining > SCRATCH_BUFSIZE ? SCRATCH_BUFSIZE : remaining;
        size_t chunksize = fread(chunk, 1, to_read, fd);
        if (chunksize == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(fd);
            logger_loge(TAG, "File sending failed!");
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
            return ESP_FAIL;
        }
        remaining -= chunksize;
    }

    /* Close file after sending complete */
    fclose(fd);
    logger_logi(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Handler to upload a file onto the server */
static esp_err_t send_upload_error(httpd_req_t *req, httpd_err_code_t status, const char *msg)
{
    const char *status_str = NULL;
    switch (status) {
        case HTTPD_400_BAD_REQUEST:
            status_str = "400 Bad Request";
            break;
        case HTTPD_500_INTERNAL_SERVER_ERROR:
            status_str = "500 Internal Server Error";
            break;
        case HTTPD_413_CONTENT_TOO_LARGE:
            status_str = "413 Content Too Large";
            break;
        default:
            break;
    }
    if (status_str) {
        httpd_resp_set_status(req, status_str);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg ? msg : "Upload failed");
    return ESP_FAIL;
}

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    /* Skip leading "/file-server" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/file-server") - 1, sizeof(filepath));
    if (!filename) {
        /* Respond with 500 Internal Server Error */
        return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
    }

    size_t filename_len = strlen(filename);

    /* Filename must exist and cannot have a trailing '/' */
    if (filename_len == 0 || filename[filename_len - 1] == '/') {
        logger_loge(TAG, "Invalid filename : %s", filename);
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    }

    const char *base_name = filename;
    const char *slash_pos = strrchr(filename, '/');
    if (slash_pos && *(slash_pos + 1)) {
        base_name = slash_pos + 1;
    }

    if (!is_audio_filename(base_name)) {
        logger_loge(TAG, "Rejected non-audio upload : %s", base_name);
        return send_upload_error(req, HTTPD_400_BAD_REQUEST,
                                 "Only audio files are allowed (" ALLOWED_AUDIO_EXTS_LIST ")");
    }

    if (stat(filepath, &file_stat) == 0) {
        logger_loge(TAG, "File already exists : %s", filepath);
        /* Respond with 400 Bad Request */
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "File already exists");
    }

    if (files_count() >= FILES_MAX_ENTRIES) {
        logger_loge(TAG, "Max file entries reached");
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "File entry limit reached");
    }

    /* File cannot be larger than a limit */
    if (req->content_len > MAX_FILE_SIZE) {
        logger_loge(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 400 Bad Request */
        return send_upload_error(req, HTTPD_413_CONTENT_TOO_LARGE,
                                 "File size must be less than " MAX_FILE_SIZE_STR "!");
        /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
    }

    fd = fopen(filepath, "wb");
    if (!fd) {
        logger_loge(TAG, "Failed to create file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
    }

    file_entry_t meta;
    file_entry_init(&meta, base_name);
    file_entry_set_props(&meta, base_name, 100, 100, true);
    meta.data_size = (size_t)req->content_len;
    meta.modified = false; // freshly written

    if (files_write_header(fd, &meta) != ESP_OK) {
        fclose(fd);
        unlink(filepath);
        return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write header");
    }

    logger_logi(TAG, "Receiving file : %s...", filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {

        logger_logi(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            fclose(fd);
            unlink(filepath);

            logger_loge(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
        }

        /* Write buffer content to file on storage */
        if (received && (received != fwrite(buf, 1, received, fd))) {
            /* Couldn't write everything to file!
             * Storage may be full? */
            fclose(fd);
            unlink(filepath);

            logger_loge(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
        }

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

    /* Close file upon upload completion */
    fclose(fd);
    logger_logi(TAG, "File reception complete");

    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/file-server/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return (sscanf(p, "%d", out) == 1);
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncasecmp(p, "true", 4))  { *out = true;  return true; }
    if (!strncasecmp(p, "false", 5)) { *out = false; return true; }
    int v = 0;
    if (sscanf(p, "%d", &v) == 1) { *out = (v != 0); return true; }
    return false;
}

static esp_err_t file_meta_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct stat st;

    const char *filename = get_path_from_uri(
        filepath,
        ((struct file_server_data *)req->user_ctx)->base_path,
        req->uri + sizeof("/file-meta") - 1,
        sizeof(filepath)
    );
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    size_t filename_len = strlen(filename);
    if (filename_len == 0 || filename[filename_len - 1] == '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    if (stat(filepath, &st) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    const char *base_name = filename;
    const char *slash_in_filename = strrchr(filename, '/');
    if (slash_in_filename && *(slash_in_filename + 1)) {
        base_name = slash_in_filename + 1;
    }

    file_entry_t meta;
    if (files_read_header(base_name, &meta) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File entry missing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
                         "{\"probability\":%u,\"volume\":%u,\"enabled\":%s}\n",
                         (unsigned)meta.props.probability,
                         (unsigned)meta.props.volume,
                         meta.props.enabled ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, n);
        return ESP_OK;
    }

    if (req->method == HTTP_POST) {
        // small JSON body expected
        int len = req->content_len;
        if (len <= 0 || len > 256) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON size");
            return ESP_FAIL;
        }

        char body[257];
        int received = 0;
        while (received < len) {
            int r = httpd_req_recv(req, body + received, len - received);
            if (r <= 0) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
                return ESP_FAIL;
            }
            received += r;
        }
        body[len] = '\0';

        int p = (int)meta.props.probability;
        int v = (int)meta.props.volume;
        bool e = meta.props.enabled;

        // keys must match what the browser sends
        json_get_int(body, "probability", &p);
        json_get_int(body, "volume", &v);
        json_get_bool(body, "enabled", &e);

        if (p < 0) {
            p = 0;
        }
        if (p > 100) {
            p = 100;
        }
        if (v < 0) {
            v = 0;
        }
        if (v > 100) {
            v = 100;
        }

        file_entry_set_props(&meta, NULL, (uint8_t)p, (uint8_t)v, e);
        FILE *fd = fopen(filepath, "r+b");
        if (!fd) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
            return ESP_FAIL;
        }
        if (fseek(fd, 0, SEEK_SET) != 0 || files_write_header(fd, &meta) != ESP_OK) {
            fclose(fd);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save properties");
            return ESP_FAIL;
        }
        fclose(fd);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

/* Function to start the file server */
esp_err_t start_ws_server(const char *base_path)
{
    static struct file_server_data *server_data = NULL;

    if (server_data) {
        logger_loge(TAG, "File server already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate memory for server data */
    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) {
        logger_loge(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(server_data->base_path, base_path,
            sizeof(server_data->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    logger_logi(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        logger_loge(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    httpd_uri_t root_index = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = http_resp_index_html,
        .user_ctx  = NULL
    };
    httpd_uri_t navbar_js = {
        .uri       = "/navbar.js",
        .method    = HTTP_GET,
        .handler   = http_resp_navbar_js,
        .user_ctx  = NULL
    };
    /* URI handler for getting uploaded files */
    httpd_uri_t file_download = {
        .uri       = "/file-server/*",  // Match all URIs of type /path/to/file
        .method    = HTTP_GET,
        .handler   = download_get_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &root_index);
    httpd_register_uri_handler(server, &navbar_js);
    httpd_register_uri_handler(server, &file_download);

    /* URI handler for uploading files to server (same path space as downloads) */
    httpd_uri_t file_upload = {
        .uri       = "/file-server/*",   // Match all URIs of type /file-server/path/to/file
        .method    = HTTP_POST,
        .handler   = upload_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_upload);

    httpd_uri_t ota_update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_update_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_update);

    httpd_uri_t logs_get = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = http_resp_logs,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &logs_get);

    httpd_uri_t mt_page = {
        .uri = "/mt",
        .method = HTTP_GET,
        .handler = http_resp_mt_html,
        .user_ctx = NULL
    };
    httpd_uri_t mt_page_slash = {
        .uri = "/mt/",
        .method = HTTP_GET,
        .handler = http_resp_mt_html,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &mt_page);
    httpd_register_uri_handler(server, &mt_page_slash);

    httpd_uri_t format_storage_uri = {
        .uri = "/format",
        .method = HTTP_POST,
        .handler = format_storage_handler,
        .user_ctx = server_data
    };
    httpd_register_uri_handler(server, &format_storage_uri);

    httpd_uri_t file_meta = {
        .uri = "/file-meta/*",
        .method = HTTP_ANY,
        .handler = file_meta_handler,
        .user_ctx = server_data
    };
    httpd_register_uri_handler(server, &file_meta);

    return ESP_OK;
}
