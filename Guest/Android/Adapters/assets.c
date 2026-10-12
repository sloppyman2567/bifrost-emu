#include "native_activity.h"
struct AAssetManager {
    char *path;
};
struct AAsset {
    int fd;
    long length;
    void *buffer;
};
AAssetManager *bifrost_asset_manager_create(const char *path) {
    AAssetManager *manager = calloc(1, sizeof(*manager));
    if (!manager)
        return 0;
    size_t n = strlen(path) + 1;
    manager->path = malloc(n);
    if (!manager->path) {
        free(manager);
        return 0;
    }
    memcpy(manager->path, path, n);
    return manager;
}
void bifrost_asset_manager_destroy(AAssetManager *manager) {
    if (manager) {
        free(manager->path);
        free(manager);
    }
}
AAsset *AAssetManager_open(AAssetManager *manager, const char *name, int mode) {
    if (!manager || !name || !*name || *name == '/' || mode < 0 || mode > 3)
        return 0;
    const char *start = name;
    for (const char *p = name;; ++p)
        if (*p == '/' || !*p) {
            size_t n = p - start;
            if (!n || (n == 1 && start[0] == '.') || (n == 2 && start[0] == '.' && start[1] == '.'))
                return 0;
            if (!*p)
                break;
            start = p + 1;
        }
    size_t n = strlen(manager->path) + strlen(name) + 2;
    if (n > 4096)
        return 0;
    char *path = malloc(n);
    if (!path)
        return 0;
    snprintf(path, n, "%s/%s", manager->path, name);
    int fd = open(path, 0x80000);
    free(path);
    if (fd < 0)
        return 0;
    long length = lseek(fd, 0, 2);
    if (length < 0 || length > 64 * 1024 * 1024 || lseek(fd, 0, 0) < 0) {
        close(fd);
        return 0;
    }
    AAsset *asset = calloc(1, sizeof(*asset));
    if (!asset) {
        close(fd);
        return 0;
    }
    asset->fd = fd;
    asset->length = length;
    return asset;
}
int AAsset_read(AAsset *asset, void *buffer, size_t size) {
    if (!asset || size > 0x7fffffff)
        return -1;
    return (int)read(asset->fd, buffer, size);
}
long AAsset_seek(AAsset *asset, long offset, int whence) {
    return asset ? lseek(asset->fd, offset, whence) : -1;
}
long AAsset_getLength(AAsset *asset) { return asset ? asset->length : 0; }
const void *AAsset_getBuffer(AAsset *asset) {
    if (!asset)
        return 0;
    if (!asset->buffer) {
        asset->buffer = malloc(asset->length ? (size_t)asset->length : 1);
        if (!asset->buffer)
            return 0;
        long position = lseek(asset->fd, 0, 1);
        long used = 0;
        if (position < 0 || lseek(asset->fd, 0, 0) < 0) {
            free(asset->buffer);
            asset->buffer = 0;
            return 0;
        }
        while (used < asset->length) {
            long n = read(asset->fd, (char *)asset->buffer + used, (size_t)(asset->length - used));
            if (n <= 0)
                break;
            used += n;
        }
        if (lseek(asset->fd, position, 0) < 0 || used != asset->length) {
            free(asset->buffer);
            asset->buffer = 0;
        }
    }
    return asset->buffer;
}
void AAsset_close(AAsset *asset) {
    if (asset) {
        close(asset->fd);
        free(asset->buffer);
        free(asset);
    }
}
