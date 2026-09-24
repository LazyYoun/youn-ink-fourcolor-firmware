/**
 * @file photo_storage.cc
 * @brief SPIFFS-based photo storage implementation
 *
 * Uses POSIX file I/O on the "assets" SPIFFS partition.
 * Index file "photos.idx" stores a simple binary header + PhotoInfo records.
 */

#include "photo_storage.h"

#include "i18n.h"

#include <esp_log.h>
#include <esp_spiffs.h>
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

static const char* kTag = "PhotoStorage";

// SPIFFS mount point and partition label (must match partition table)
static const char* kBasePath = "/spiffs";
static const char* kPartitionLabel = "assets";
// SPIFFS is flat (no real directories). Keep photo files directly under mount path.
static const char* kPhotoDir = "/spiffs";
static const char* kIndexFile = "/spiffs/photos.idx";

// Index file format:
// Bytes 0-3: magic "PHOT"
// Bytes 4-5: version (uint16)
// Bytes 6-7: count (uint16)
// Then N * sizeof(PhotoInfo) records

#define INDEX_MAGIC 0x50484F54  // "PHOT"
#define INDEX_VERSION 2

static PhotoInfo s_photos[PHOTO_MAX_PHOTOS];
static int s_photo_count = 0;
static bool s_initialized = false;

static int save_index(void);
static int write_meta_file(const PhotoInfo* info);

static bool is_digits_string(const char* value) {
    if (!value || value[0] == '\0') return false;
    for (const char* p = value; *p; ++p) {
        if (!isdigit(static_cast<unsigned char>(*p))) return false;
    }
    return true;
}

static void format_epoch_date(uint64_t epoch, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (epoch > 100000000000ULL) {
        epoch /= 1000;
    }
    time_t ts = static_cast<time_t>(epoch);
    struct tm tm_buf;
    if (localtime_r(&ts, &tm_buf) == nullptr) {
        return;
    }
    strftime(out, out_size, "%Y-%m-%d", &tm_buf);
}

static void apply_default_metadata(PhotoInfo* info) {
    if (!info) return;
    if (info->title[0] == '\0') {
        strlcpy(info->title, i18n::Tr(i18n::StringId::kOnThisDay), sizeof(info->title));
    }
    if (info->date[0] == '\0' && info->timestamp > 0) {
        format_epoch_date(info->timestamp, info->date, sizeof(info->date));
    } else if (is_digits_string(info->date) && strlen(info->date) >= 9) {
        format_epoch_date(strtoull(info->date, nullptr, 10), info->date, sizeof(info->date));
    }
    if (info->location[0] == '\0') {
        strlcpy(info->location, i18n::Tr(i18n::StringId::kUnknownLocation), sizeof(info->location));
    }
    if (info->body[0] == '\0') {
        strlcpy(info->body, i18n::Tr(i18n::StringId::kNoCaptionYet), sizeof(info->body));
    }
}

static bool load_meta_file(const char* meta_path, PhotoInfo* out_info) {
    if (!meta_path || !out_info) return false;

    FILE* f = fopen(meta_path, "rb");
    if (!f) {
        ESP_LOGW(kTag, "Failed to open meta file: %s", meta_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096) {
        fclose(f);
        ESP_LOGW(kTag, "Invalid meta file size: %s", meta_path);
        return false;
    }

    char json_buf[4096];
    size_t read_len = fread(json_buf, 1, static_cast<size_t>(len), f);
    fclose(f);
    if (read_len != static_cast<size_t>(len)) {
        ESP_LOGW(kTag, "Short read for meta file: %s", meta_path);
        return false;
    }
    json_buf[read_len] = '\0';

    cJSON* root = cJSON_Parse(json_buf);
    if (!root) {
        ESP_LOGW(kTag, "Failed to parse meta JSON: %s", meta_path);
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));
    cJSON* item = nullptr;

    item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out_info->id, item->valuestring, sizeof(out_info->id));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out_info->title, item->valuestring, sizeof(out_info->title));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "date");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out_info->date, item->valuestring, sizeof(out_info->date));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "location");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out_info->location, item->valuestring, sizeof(out_info->location));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out_info->body, item->valuestring, sizeof(out_info->body));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "width");
    if (cJSON_IsNumber(item)) out_info->width = static_cast<uint16_t>(item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(root, "height");
    if (cJSON_IsNumber(item)) out_info->height = static_cast<uint16_t>(item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(root, "file_size");
    if (cJSON_IsNumber(item)) out_info->file_size = static_cast<uint32_t>(item->valuedouble);
    item = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    if (cJSON_IsNumber(item)) out_info->timestamp = static_cast<uint32_t>(item->valuedouble);
    item = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out_info->path, item->valuestring, sizeof(out_info->path));
    }

    cJSON_Delete(root);
    apply_default_metadata(out_info);
    return out_info->id[0] != '\0';
}

static int rebuild_index_from_meta_files(void) {
    DIR* dir = opendir(kPhotoDir);
    if (!dir) {
        ESP_LOGW(kTag, "Failed to open photo dir for rebuild");
        return -1;
    }

    s_photo_count = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr && s_photo_count < PHOTO_MAX_PHOTOS) {
        const char* name = entry->d_name;
        size_t name_len = strlen(name);
        if (name_len < 6 || strcasecmp(name + name_len - 5, ".meta") != 0) {
            continue;
        }

        char meta_path[320];
        snprintf(meta_path, sizeof(meta_path), "%s/%s", kPhotoDir, name);

        PhotoInfo info;
        if (!load_meta_file(meta_path, &info)) {
            continue;
        }

        if (info.path[0] == '\0') {
            snprintf(info.path, sizeof(info.path), "%s/%s.bin", kPhotoDir, info.id);
        }
        s_photos[s_photo_count++] = info;
    }
    closedir(dir);

    save_index();
    ESP_LOGI(kTag, "Rebuilt photo index from meta files: %d photos", s_photo_count);
    return s_photo_count;
}

// Ensure SPIFFS mount path is available.
static bool ensure_photo_dir(void) {
    struct stat st;
    if (stat(kBasePath, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            ESP_LOGW(kTag, "Base path exists but is not a directory: %s", kBasePath);
        }
        return true;
    }

    // Some SPIFFS setups don't expose a stat-able mount root despite successful
    // registration. Keep storage enabled and rely on direct file open operations.
    ESP_LOGW(kTag, "Base path stat unavailable (%s), continue in flat-file mode", kBasePath);
    return true;
}

// Write index file
static int save_index(void) {
    FILE *f = fopen(kIndexFile, "wb");
    if (!f) {
        ESP_LOGE(kTag, "Failed to open index for writing");
        return -1;
    }

    // Write header
    uint32_t magic = INDEX_MAGIC;
    uint16_t version = INDEX_VERSION;
    uint16_t count = (uint16_t)s_photo_count;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&count, sizeof(count), 1, f);

    // Write records
    if (s_photo_count > 0) {
        fwrite(s_photos, sizeof(PhotoInfo), s_photo_count, f);
    }

    fclose(f);
    return 0;
}

static int write_meta_file(const PhotoInfo* info) {
    if (!info || info->id[0] == '\0') return -1;

    char meta_path[PHOTO_MAX_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", kPhotoDir, info->id);

    FILE* f = fopen(meta_path, "w");
    if (!f) {
        ESP_LOGE(kTag, "Failed to open %s for writing", meta_path);
        return -1;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        fclose(f);
        return -1;
    }
    cJSON_AddStringToObject(root, "id", info->id);
    cJSON_AddStringToObject(root, "title", info->title);
    cJSON_AddStringToObject(root, "date", info->date);
    cJSON_AddStringToObject(root, "location", info->location);
    cJSON_AddStringToObject(root, "body", info->body);
    cJSON_AddNumberToObject(root, "width", info->width);
    cJSON_AddNumberToObject(root, "height", info->height);
    cJSON_AddNumberToObject(root, "file_size", info->file_size);
    cJSON_AddNumberToObject(root, "timestamp", (double)info->timestamp);
    cJSON_AddStringToObject(root, "path", info->path);

    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        fputs(json_str, f);
        free(json_str);
    }
    cJSON_Delete(root);
    fclose(f);
    return json_str ? 0 : -1;
}

// Read index file
static int load_index(void) {
    FILE *f = fopen(kIndexFile, "rb");
    if (!f) {
        // No index yet, start fresh
        s_photo_count = 0;
        return 0;
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t count = 0;

    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        fread(&count, sizeof(count), 1, f) != 1) {
        fclose(f);
        ESP_LOGE(kTag, "Failed to read index header");
        s_photo_count = 0;
        return -1;
    }

    if (magic != INDEX_MAGIC || version != INDEX_VERSION) {
        fclose(f);
        ESP_LOGW(kTag, "Index version mismatch (magic=0x%08X ver=%d), rebuilding", magic, version);
        s_photo_count = 0;
        return rebuild_index_from_meta_files();
    }

    if (count > PHOTO_MAX_PHOTOS) {
        count = PHOTO_MAX_PHOTOS;
    }

    s_photo_count = 0;
    for (int i = 0; i < count; i++) {
        if (fread(&s_photos[i], sizeof(PhotoInfo), 1, f) == 1) {
            // Validate record
            if (s_photos[i].id[0] != '\0' && s_photos[i].path[0] != '\0') {
                apply_default_metadata(&s_photos[i]);
                s_photo_count++;
            }
        }
    }

    fclose(f);
    ESP_LOGI(kTag, "Loaded %d photos from index", s_photo_count);
    return 0;
}

int photo_storage_init(void) {
    if (s_initialized) {
        ESP_LOGW(kTag, "Already initialized");
        return 0;
    }

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = kBasePath,
        .partition_label = kPartitionLabel,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return -1;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(kPartitionLabel, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "SPIFFS mounted: total=%d KB, used=%d KB", total / 1024, used / 1024);
    }

    // Create photo directory
    if (!ensure_photo_dir()) {
        return -1;
    }

    // Load index
    load_index();

    s_initialized = true;
    ESP_LOGI(kTag, "Photo storage initialized (%d photos)", s_photo_count);
    return 0;
}

int photo_save(const PhotoInfo *info, const uint8_t *data_1bpp) {
    if (!s_initialized) {
        if (photo_storage_init() != 0) {
            return -1;
        }
    }
    if (!info || !data_1bpp) {
        return -1;
    }

    int existing_index = -1;
    for (int i = 0; i < s_photo_count; ++i) {
        if (strcmp(s_photos[i].id, info->id) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index < 0 && s_photo_count >= PHOTO_MAX_PHOTOS) {
        ESP_LOGE(kTag, "Photo storage full (%d)", PHOTO_MAX_PHOTOS);
        return -1;
    }

    // Write .bin file
    char bin_path[PHOTO_MAX_PATH];
    snprintf(bin_path, sizeof(bin_path), "%s/%s.bin", kPhotoDir, info->id);

    FILE *f = fopen(bin_path, "wb");
    if (!f) {
        ESP_LOGE(kTag, "Failed to open %s for writing", bin_path);
        return -1;
    }

    size_t written = fwrite(data_1bpp, 1, info->file_size, f);
    fclose(f);

    if (written != info->file_size) {
        ESP_LOGE(kTag, "Failed to write full data: %d/%d", written, info->file_size);
        return -1;
    }

    // Add to or update the in-memory index.
    PhotoInfo *entry = nullptr;
    if (existing_index >= 0) {
        entry = &s_photos[existing_index];
    } else {
        entry = &s_photos[s_photo_count];
    }
    memcpy(entry, info, sizeof(PhotoInfo));
    strncpy(entry->path, bin_path, PHOTO_MAX_PATH - 1);
    entry->path[PHOTO_MAX_PATH - 1] = '\0';
    apply_default_metadata(entry);
    if (existing_index < 0) {
        s_photo_count++;
    }

    write_meta_file(entry);

    // Persist index
    save_index();

    ESP_LOGI(kTag, "%s photo %s (%dx%d, %d bytes), total=%d",
             existing_index >= 0 ? "Updated" : "Saved",
             info->id, info->width, info->height, info->file_size, s_photo_count);
    return 0;
}

int photo_load(const char *id, uint8_t *out_buffer, uint32_t max_size) {
    if (!s_initialized || !id || !out_buffer) {
        return -1;
    }

    // Find the photo
    for (int i = 0; i < s_photo_count; i++) {
        if (strcmp(s_photos[i].id, id) == 0) {
            if (s_photos[i].file_size > max_size) {
                ESP_LOGE(kTag, "Buffer too small: %d > %d", s_photos[i].file_size, max_size);
                return -1;
            }

            FILE *f = fopen(s_photos[i].path, "rb");
            if (!f) {
                ESP_LOGE(kTag, "Failed to open %s", s_photos[i].path);
                return -1;
            }

            size_t n = fread(out_buffer, 1, s_photos[i].file_size, f);
            fclose(f);

            if (n != s_photos[i].file_size) {
                ESP_LOGE(kTag, "Read incomplete: %d/%d", n, s_photos[i].file_size);
                return -1;
            }

            return (int)n;
        }
    }

    return -1;  // Not found
}

int photo_list(PhotoInfo *out_list, int max_count) {
    if (!s_initialized || !out_list || max_count <= 0) {
        return 0;
    }

    int n = (max_count < s_photo_count) ? max_count : s_photo_count;
    for (int i = 0; i < n; i++) {
        memcpy(&out_list[i], &s_photos[i], sizeof(PhotoInfo));
    }
    return n;
}

int photo_delete(const char *id) {
    if (!s_initialized || !id) {
        return -1;
    }

    // Find the photo
    int idx = -1;
    for (int i = 0; i < s_photo_count; i++) {
        if (strcmp(s_photos[i].id, id) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        return -1;  // Not found
    }

    // Remove .bin file
    if (s_photos[idx].path[0] != '\0') {
        remove(s_photos[idx].path);
    }

    // Remove .meta file
    char meta_path[PHOTO_MAX_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", kPhotoDir, id);
    remove(meta_path);

    // Shift remaining entries down
    for (int i = idx; i < s_photo_count - 1; i++) {
        s_photos[i] = s_photos[i + 1];
    }
    s_photo_count--;

    // Persist index
    save_index();

    ESP_LOGI(kTag, "Deleted photo %s", id);
    return 0;
}

int photo_get_count(void) {
    if (!s_initialized) {
        if (photo_storage_init() != 0) {
            return 0;
        }
    }
    return s_photo_count;
}

int photo_get_by_index(int index, PhotoInfo *out) {
    if (!s_initialized) {
        if (photo_storage_init() != 0) {
            return -1;
        }
    }
    if (!out || index < 0 || index >= s_photo_count) {
        return -1;
    }
    memcpy(out, &s_photos[index], sizeof(PhotoInfo));
    return 0;
}

bool photo_exists(const char *id) {
    if (!s_initialized || !id) {
        return false;
    }
    for (int i = 0; i < s_photo_count; i++) {
        if (strcmp(s_photos[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

int photo_update_info(const char *id, const PhotoInfo *updates) {
    if (!s_initialized) {
        if (photo_storage_init() != 0) return -1;
    }
    if (!id || !updates) return -1;

    for (int i = 0; i < s_photo_count; ++i) {
        if (strcmp(s_photos[i].id, id) != 0) continue;

        strlcpy(s_photos[i].title, updates->title, sizeof(s_photos[i].title));
        strlcpy(s_photos[i].date, updates->date, sizeof(s_photos[i].date));
        strlcpy(s_photos[i].location, updates->location, sizeof(s_photos[i].location));
        strlcpy(s_photos[i].body, updates->body, sizeof(s_photos[i].body));
        apply_default_metadata(&s_photos[i]);
        if (write_meta_file(&s_photos[i]) != 0) return -1;
        save_index();
        ESP_LOGI(kTag, "Updated photo metadata %s", id);
        return 0;
    }
    return -1;
}

int photo_move(const char *id, int delta) {
    if (!s_initialized) {
        if (photo_storage_init() != 0) return -1;
    }
    if (!id || delta == 0) return -1;

    int idx = -1;
    for (int i = 0; i < s_photo_count; ++i) {
        if (strcmp(s_photos[i].id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    const int target = idx + (delta < 0 ? -1 : 1);
    if (target < 0 || target >= s_photo_count) return -1;

    PhotoInfo tmp = s_photos[idx];
    s_photos[idx] = s_photos[target];
    s_photos[target] = tmp;
    save_index();
    ESP_LOGI(kTag, "Moved photo %s from %d to %d", id, idx, target);
    return 0;
}
