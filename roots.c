/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#ifndef major
# include <sys/sysmacros.h>
#endif

#include "mtdutils/mtdutils.h"
#include "mounts.h"
#include "roots.h"
#include "common.h"
#include "make_ext4fs.h"

#include "flashutils/flashutils.h"
#include "extendedcommands.h"

int num_volumes;
Volume* device_volumes;

int get_num_volumes() {
    return num_volumes;
}

Volume* get_device_volumes() {
    return device_volumes;
}

static int is_null(const char* sz) {
    if (sz == NULL)
        return 1;
    if (strcmp("NULL", sz) == 0)
        return 1;
    return 0;
}

static char* dupe_string(const char* sz) {
    if (is_null(sz))
        return NULL;
    return strdup(sz);
}

static int parse_options(char* options, Volume* volume) {
    char* option;
    while (option = strtok(options, ",")) {
        options = NULL;

        if (strncmp(option, "length=", 7) == 0) {
            volume->length = strtoll(option+7, NULL, 10);
        } else if (strncmp(option, "fstype2=", 8) == 0) {
            volume->fs_type2 = volume->fs_type;
            volume->fs_type = strdup(option + 8);
        } else if (strncmp(option, "fs_options=", 11) == 0) {
            volume->fs_options = strdup(option + 11);
        } else if (strncmp(option, "fs_options2=", 12) == 0) {
            volume->fs_options2 = strdup(option + 12);
        } else if (strncmp(option, "lun=", 4) == 0) {
            volume->lun = strdup(option + 4);
        } else {
            LOGE("bad option \"%s\"\n", option);
            return -1;
        }
    }
    return 0;
}

void load_volume_table() {
    int alloc = 2;
    device_volumes = malloc(alloc * sizeof(Volume));

    // Insert an entry for /tmp, which is the ramdisk and is always mounted.
    device_volumes[0].mount_point = "/tmp";
    device_volumes[0].fs_type = "ramdisk";
    device_volumes[0].device = NULL;
    device_volumes[0].device2 = NULL;
    device_volumes[0].fs_type2 = NULL;
    device_volumes[0].fs_options = NULL;
    device_volumes[0].fs_options2 = NULL;
    device_volumes[0].lun = NULL;
    device_volumes[0].length = 0;
    num_volumes = 1;

    FILE* fstab = fopen("/etc/recovery.fstab", "r");
    if (fstab == NULL) {
        LOGE("failed to open /etc/recovery.fstab (%s)\n", strerror(errno));
        return;
    }

    char buffer[1024];
    int i;
    while (fgets(buffer, sizeof(buffer)-1, fstab)) {
        for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
        if (buffer[i] == '\0' || buffer[i] == '#') continue;

        char* original = strdup(buffer);

        char* mount_point = strtok(buffer+i, " \t\n");
        char* fs_type = strtok(NULL, " \t\n");
        char* device = strtok(NULL, " \t\n");
        // lines may optionally have a second device, to use if
        // mounting the first one fails.
        char* options = NULL;
        char* device2 = strtok(NULL, " \t\n");
        if (device2) {
            if (device2[0] == '/') {
                options = strtok(NULL, " \t\n");
            } else {
                options = device2;
                device2 = NULL;
            }
        }

        if (mount_point && fs_type && device) {
            while (num_volumes >= alloc) {
                alloc *= 2;
                device_volumes = realloc(device_volumes, alloc*sizeof(Volume));
            }

            if(is_dualsystem() && strcmp(mount_point, "/data")==0) {
                char resolved_path[PATH_MAX];
                ssize_t len;

                // resolve symlink
                if((len = readlink(device, resolved_path, sizeof(resolved_path)-1)) != -1)
                    resolved_path[len] = '\0';
                else sprintf(resolved_path, "%s", device);

                // delete node
                if(rename(resolved_path, "/dev/userdata_moved")!=0)
                    LOGE("could not move %s to %s!\n", resolved_path, "/dev/userdata_moved");

                device = "/dev/userdata_moved";

            }

            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
            device_volumes[num_volumes].device = strdup(device);
            device_volumes[num_volumes].device2 =
                device2 ? strdup(device2) : NULL;

            device_volumes[num_volumes].length = 0;

            device_volumes[num_volumes].fs_type2 = NULL;
            device_volumes[num_volumes].fs_options = NULL;
            device_volumes[num_volumes].fs_options2 = NULL;
            device_volumes[num_volumes].lun = NULL;

            int code;
            if(code=stat(device, &device_volumes[num_volumes].stat)!=0)
                LOGE("stat: Error %d on file %s\n", code, device);

            if (parse_options(options, device_volumes + num_volumes) != 0) {
                LOGE("skipping malformed recovery.fstab line: %s\n", original);
            } else {
                ++num_volumes;

                if(is_dualsystem() && mount_point!=NULL && strcmp(mount_point, "/data")==0) {
                    while (num_volumes >= alloc) {
                        alloc *= 2;
                        device_volumes = realloc(device_volumes, alloc*sizeof(Volume));
                    }

                    device_volumes[num_volumes].mount_point = "/data1";
                    device_volumes[num_volumes].fs_type = "bind";
                    device_volumes[num_volumes].device = NULL;
                    device_volumes[num_volumes].device2 = NULL;
                    device_volumes[num_volumes].fs_type2 = NULL;
                    device_volumes[num_volumes].fs_options = NULL;
                    device_volumes[num_volumes].fs_options2 = NULL;
                    device_volumes[num_volumes].lun = NULL;
                    device_volumes[num_volumes].length = 0;
                    ++num_volumes;
                }
            }
        } else {
            LOGE("skipping malformed recovery.fstab line: %s\n", original);
        }
        free(original);
    }

    fclose(fstab);

    fprintf(stderr, "recovery filesystem table\n");
    fprintf(stderr, "=========================\n");
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = &device_volumes[i];
        fprintf(stderr, "  %d %s %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->device, v->device2, v->length);
    }
    fprintf(stderr,"\n");
}

Volume* volume_for_path(const char* path) {
    int i;
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = device_volumes+i;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            return v;
        }
    }
    return NULL;
}

int try_mount(const char* device, const char* mount_point, const char* fs_type, const char* fs_options) {
    if (device == NULL || mount_point == NULL || fs_type == NULL)
        return -1;
    int ret = 0;
    if (fs_options == NULL) {
        ret = mount(device, mount_point, fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
    }
    else {
        char mount_cmd[PATH_MAX];
        sprintf(mount_cmd, "mount -t %s -o%s %s %s", fs_type, fs_options, device, mount_point);
        ret = __system(mount_cmd);
    }
    if (ret == 0)
        return 0;
    LOGW("failed to mount %s (%s)\n", device, strerror(errno));
    return ret;
}

int replace_device_node(Volume* vol, struct stat* stat) {
    if(stat==NULL) return -1;

    ssize_t len;
    char resolved_path[PATH_MAX];
    if((len = readlink(vol->device, resolved_path, sizeof(resolved_path)-1)) != -1)
        resolved_path[len] = '\0';
    else sprintf(resolved_path, "%s", vol->device);

    if(ensure_path_unmounted(vol->mount_point)!=0) {
        LOGE("replace_device_node: could not unmount device!\n");
        return -1;
    } if(unlink(resolved_path)!=0) {
        LOGE("replace_device_node: could not delete node!\n");
        return -1;
    } if(mknod(resolved_path, stat->st_mode, stat->st_rdev)!=0) {
        LOGE("replace_device_node: could not create node!\n");
        return -1;
    }
    return 0;
}

int handle_volume_request(Volume* vol0, Volume* vol1, int num) {
    if(vol0!=NULL && vol1!=NULL) {
        Volume* v0;
        Volume* v1;
        if(num==DUALBOOT_ITEM_SYSTEM0) {
            v0=vol0;
            v1=vol0;
        }
        else if(num==DUALBOOT_ITEM_SYSTEM1) {
            v0=vol1;
            v1=vol1;
        }
        else if(num==DUALBOOT_ITEM_BOTH) {
            v0=vol0;
            v1=vol1;
        }
        else if(num==DUALBOOT_ITEM_INTERCHANGED) {
            v0=vol1;
            v1=vol0;
        }
        else {
            LOGE("set_active_system: invalid system number: %d!\n", num);
            return -1;
        }

        if(replace_device_node(vol0, &v0->stat)!=0)
            return -1;
        if(replace_device_node(vol1, &v1->stat)!=0)
            return -1;

        return 0;
    }
    else {
        LOGE("set_active_system: invalid volumes given!\n");
        return -1;
    }
}

int selected_dualsystem_mode = -1;
int getDualsystemMode() {
    return selected_dualsystem_mode;
}

int set_active_system(int num) {
    int i;
    char* mount_point;
    Volume* system0 = volume_for_path("/system");
    Volume* system1 = volume_for_path("/system1");
    Volume* boot0 = volume_for_path("/boot");
    Volume* boot1 = volume_for_path("/boot1");
    Volume* radio0 = volume_for_path("/radio");
    Volume* radio1 = volume_for_path("/radio1");

    handle_volume_request(system0, system1, num);
    handle_volume_request(boot0, boot1, num);
    handle_volume_request(radio0, radio1, num);

    if(ensure_path_unmounted("/data")!=0) {
        LOGE("could not unmount /data!\n");
        return -1;
    }

    selected_dualsystem_mode = num;

    return 0;
}

int is_dualsystem() {
    int i;
    for (i = 0; i < num_volumes; i++) {
        Volume* vol = device_volumes + i;
        if (strcmp(vol->mount_point, "/system1") == 0)
            return 1;
    }
    return 0;
}

int is_data_media() {
    int i;
    for (i = 0; i < num_volumes; i++) {
        Volume* vol = device_volumes + i;
        if (strcmp(vol->fs_type, "datamedia") == 0)
            return 1;
    }
    return 0;
}

void setup_data_media() {
    int i;
    for (i = 0; i < num_volumes; i++) {
        Volume* vol = device_volumes + i;
        if (strcmp(vol->fs_type, "datamedia") == 0) {
            rmdir(vol->mount_point);
            mkdir("/data/media", 0755);
            symlink("/data/media", vol->mount_point);
            return;
        }
    }
}

int is_data_media_volume_path(const char* path) {
    Volume* v = volume_for_path(path);
    return strcmp(v->fs_type, "datamedia") == 0;
}

int ensure_path_mounted(const char* path) {
    return ensure_path_mounted_at_mount_point(path, NULL);
}

int lastDataSubfolder = -1;
int getLastDataSubfolder() {
    return lastDataSubfolder;
}

int ensure_path_mounted_at_mount_point(const char* path, const char* mount_point) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (is_data_media_volume_path(path)) {
        if (ui_should_log_stdout()) {
            LOGI("using /data/media for %s.\n", path);
        }
        int ret;
        if (0 != (ret = ensure_path_mounted("/data")))
            return ret;
        setup_data_media();
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 0;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    if (NULL == mount_point)
        mount_point = v->mount_point;

    int dataNum = -1;
    if(strcmp(path, "/data") == 0)
        dataNum = 0;
    else if(strcmp(path, "/data1") == 0)
        dataNum = 1;

    if (dataNum>=0 && is_dualsystem() && strcmp(mount_point, DUALBOOT_PATH_DATAROOT) != 0 && isTrueDualbootEnabled()) {
        int ret;
        int subfolder[2];

        if (0 != (ret = ensure_path_mounted_at_mount_point("/data", DUALBOOT_PATH_DATAROOT)))
            return ret;

        // get subfolder for bind-mount
        switch(getDualsystemMode()) {
            case DUALBOOT_ITEM_SYSTEM0:
                subfolder[0] = DUALBOOT_ITEM_SYSTEM0;
                subfolder[1] = DUALBOOT_ITEM_SYSTEM0;
            break;

            case DUALBOOT_ITEM_SYSTEM1:
                subfolder[0] = DUALBOOT_ITEM_SYSTEM1;
                subfolder[1] = DUALBOOT_ITEM_SYSTEM1;
            break;

            case DUALBOOT_ITEM_BOTH:
            case -1:
                subfolder[0] = DUALBOOT_ITEM_SYSTEM0;
                subfolder[1] = DUALBOOT_ITEM_SYSTEM1;
            break;

            case DUALBOOT_ITEM_INTERCHANGED:
                subfolder[0] = DUALBOOT_ITEM_SYSTEM1;
                subfolder[1] = DUALBOOT_ITEM_SYSTEM0;
            break;

            default:
                LOGI("Unsupported DualsystemMode for mounting data: %d\n", getDualsystemMode());
                return -1;
        }

        // check if volume is already mounted
        const MountedVolume* mv =
            find_mounted_volume_by_mount_point("/data");
        if (mv) {
            // volume is already mounted
            if(lastDataSubfolder>=0) {
                if(lastDataSubfolder!=subfolder[dataNum] && ensure_path_unmounted("/data") != 0) {
                    LOGE("failed to unmount \"/data\"\n");
                    return -1;
                }
            }
            else return 0;
        }

        // create directories
        mkdir(mount_point, 0755);
        mkdir(DUALBOOT_PATH_USERDATA0, 0755);
        mkdir(DUALBOOT_PATH_USERDATA1, 0755);

        // get sSubfolder
        char* sSubfolder;
        if(subfolder[dataNum]==DUALBOOT_ITEM_SYSTEM0)
            sSubfolder = DUALBOOT_PATH_USERDATA0;
        else if(subfolder[dataNum]==DUALBOOT_ITEM_SYSTEM1)
            sSubfolder = DUALBOOT_PATH_USERDATA1;

        // bind-mount /data
        char bindmount_command[PATH_MAX];
        sprintf(bindmount_command, "mount -o bind %s %s", sSubfolder, mount_point);
        ret = __system(bindmount_command);
        if(ret!=0) return ret;

        lastDataSubfolder=subfolder[dataNum];
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(mount_point);
    if (mv) {
        // volume is already mounted
        return 0;
    }

    mkdir(mount_point, 0755);  // in case it doesn't already exist

    if (strcmp(v->fs_type, "yaffs2") == 0) {
        // mount an MTD partition as a YAFFS2 filesystem.
        mtd_scan_partitions();
        const MtdPartition* partition;
        partition = mtd_find_partition_by_name(v->device);
        if (partition == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->device, mount_point);
            return -1;
        }
        return mtd_mount_partition(partition, mount_point, v->fs_type, 0);
    } else if (strcmp(v->fs_type, "ext4") == 0 ||
               strcmp(v->fs_type, "ext3") == 0 ||
               strcmp(v->fs_type, "rfs") == 0 ||
               strcmp(v->fs_type, "vfat") == 0) {
        if ((result = try_mount(v->device, mount_point, v->fs_type, v->fs_options)) == 0)
            return 0;
        if ((result = try_mount(v->device2, mount_point, v->fs_type, v->fs_options)) == 0)
            return 0;
        if ((result = try_mount(v->device, mount_point, v->fs_type2, v->fs_options2)) == 0)
            return 0;
        if ((result = try_mount(v->device2, mount_point, v->fs_type2, v->fs_options2)) == 0)
            return 0;
        return result;
    } else {
        // let's try mounting with the mount binary and hope for the best.
        char mount_cmd[PATH_MAX];
        sprintf(mount_cmd, "mount %s", mount_point);
        return __system(mount_cmd);
    }

    LOGE("unknown fs_type \"%s\" for %s\n", v->fs_type, mount_point);
    return -1;
}
static int handle_truedualsystem = 0;
int ensure_path_unmounted(const char* path) {
    // if we are using /data/media, do not ever unmount volumes /data or /sdcard
    if (strstr(path, "/data") == path && is_data_media()) {
        return 0;
    }
    // if we are using TrueDualBoot do not ever unmount volume /data_root
    if (strstr(path, DUALBOOT_PATH_DATAROOT) == path && is_dualsystem() && isTrueDualbootEnabled()) {
        return 0;
    }

    Volume* v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (is_data_media_volume_path(path)) {
        return ensure_path_unmounted("/data");
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted; you can't unmount it.
        return -1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    // if we are NOT using TrueDualBoot do unmount volume /data_root too
    if (strstr(path, "/data") == path && is_dualsystem() && !(isTrueDualbootEnabled() && !handle_truedualsystem)) {
        const MountedVolume* mv =
            find_mounted_volume_by_mount_point(DUALBOOT_PATH_DATAROOT);
        if(mv!=NULL) {
            unmount_mounted_volume(mv);
        }
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        // volume is already unmounted
        return 0;
    }

    return unmount_mounted_volume(mv);
}

extern struct selabel_handle *sehandle;
static int handle_data_media = 0;

int format_volume(const char* volume) {
    Volume* v = volume_for_path(volume);
    if (v == NULL) {
        // silent failure for sd-ext
        if (strcmp(volume, "/sd-ext") == 0)
            return -1;
        LOGE("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (is_data_media_volume_path(volume)) {
        return format_unknown_device(NULL, volume, NULL);
    }
    // check to see if /data is being formatted, and if it is /data/media
    // Note: the /sdcard check is redundant probably, just being safe.
    if (strstr(volume, "/data") == volume && is_data_media() && !handle_data_media) {
        return format_unknown_device(NULL, volume, NULL);
    }
    if (strstr(volume, "/data") == volume && is_dualsystem() && isTrueDualbootEnabled() && !handle_truedualsystem) {
        return format_unknown_device(NULL, volume, NULL);
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", volume);
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
#if 0
        LOGE("can't give path \"%s\" to format_volume\n", volume);
        return -1;
#endif
        return format_unknown_device(v->device, volume, NULL);
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(v->fs_type, "yaffs2") == 0 || strcmp(v->fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(v->device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", v->device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", v->device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", v->device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n", v->device);
            return -1;
        }
        return 0;
    }

    if (strcmp(v->fs_type, "ext4") == 0) {
        int result = make_ext4fs(v->device, v->length, volume, sehandle);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", v->device);
            return -1;
        }
        return 0;
    }

#if 0
    LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
    return -1;
#endif
    return format_unknown_device(v->device, volume, v->fs_type);
}

void handle_data_media_format(int handle) {
  handle_data_media = handle;
}

void handle_truedualsystem_format(int handle) {
  handle_truedualsystem = handle;
}
