/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2014, The CyanogenMod Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include <linux/msm_mdp.h>
#include <linux/msm_ion.h>

#include "minui.h"
#include "graphics.h"
#include "vsync.h"

#ifdef MDSS_MDP_RIGHT_MIXER

#define MDP_V4_0 400
#define PIXEL_SIZE 4
#define ALIGN(x, align) (((x) + ((align)-1)) & ~((align)-1))
#define FB_PATH "/sys/class/graphics/fb0/name"

static gr_surface overlay_init(minui_backend*);
static gr_surface overlay_flip(minui_backend*);
static void overlay_blank(minui_backend*, bool);
static void overlay_exit(minui_backend*);

static GRSurface gr_draw;
static struct fb_var_screeninfo vi;
static int fb_fd = -1;

static minui_backend overlay_backend = {
    .init = overlay_init,
    .flip = overlay_flip,
    .blank = overlay_blank,
    .exit = overlay_exit,
};

typedef struct {
    unsigned char *mem_buf;
    int size;
    int ion_fd;
    int mem_fd;
    struct ion_handle_data handle_data;
    int offset;
    sem_t acquire;
    sem_t retire;
} ion_mem_info;

// triple buffer
#define NUM_BUFFERS 3
static ion_mem_info mem_info[NUM_BUFFERS];

//Left and right overlay id
static int overlayL_id = MSMFB_NEW_REQUEST;
static int overlayR_id = MSMFB_NEW_REQUEST;
static bool isMDP5 = false;

static int map_mdp_pixel_format()
{
    int format = MDP_RGB_565;
#if defined(RECOVERY_BGRA)
    format = MDP_BGRA_8888;
#elif defined(RECOVERY_RGBX)
    format = MDP_RGBA_8888;
#endif
    return format;
}

#define MAX_DISPLAY_DIM  2048

static int leftSplit = 0;
static int rightSplit = 0;

static void setDisplaySplit() {
    char split[64] = {0};
    FILE* fp = fopen("/sys/class/graphics/fb0/msm_fb_split", "r");
    if (fp) {
        //Format "left right" space as delimiter
        if(fread(split, sizeof(char), 64, fp)) {
            leftSplit = atoi(split);
            LOGI("Left split=%d\n",leftSplit);
            char *rght = strpbrk(split, " ");
            if (rght)
                rightSplit = atoi(rght + 1);
            LOGI("Right split=%d\n", rightSplit);
        }
    } else {
        LOGD("Failed to open mdss_fb_split node\n");
    }
    if (fp)
        fclose(fp);
}

static int getLeftSplit() {
   //Default even split for all displays with high res
   int lSplit = vi.xres / 2;

   //Override if split published by driver
   if (leftSplit)
       lSplit = leftSplit;

   return lSplit;
}

static int getRightSplit() {
   return rightSplit;
}

static bool isDisplaySplit() {
    if (vi.xres > MAX_DISPLAY_DIM)
        return true;
    //check if right split is set by driver
    if (getRightSplit())
        return true;

    return false;
}


static int getFbXres() {
    return vi.xres;
}

static int getFbYres () {
    return vi.yres;
}

bool target_has_overlay()
{
    bool ret = false;
    char version[32];
    char str_ver[4];
    int len = 0;
    int fd = open(FB_PATH, O_RDONLY);

    if (fd < 0)
        return false;

    if ((len = read(fd, version, 31)) >= 0) {
        version[len] = '\0';
    }
    close(fd);

    if (len >= 8) {
        if (!strncmp(version, "mdssfb", strlen("mdssfb"))) {
            ret = true;
            isMDP5 = true;
        }
    }

    return ret;
}

minui_backend* open_overlay() {
    return &overlay_backend;
}

static bool isTargetMdp5() {
    if (isMDP5)
        return true;

    return false;
}

static int free_ion_mem(void) {
    int ret = 0, i = 0;

    for (i = 0; i < NUM_BUFFERS; i++) {
        if (mem_info[i].mem_buf)
            munmap(mem_info[i].mem_buf, mem_info[i].size);

        if (mem_info[i].ion_fd >= 0) {
            ret = ioctl(mem_info[i].ion_fd, ION_IOC_FREE, &mem_info[i].handle_data);
            if (ret < 0) {
                perror("free_mem failed ");
                continue;
            }
        }

        if (mem_info[i].mem_fd >= 0)
            close(mem_info[i].mem_fd);
        if (mem_info[i].ion_fd >= 0)
            close(mem_info[i].ion_fd);

        memset(&mem_info[i], 0, sizeof(ion_mem_info));
        mem_info[i].mem_fd = -1;
        mem_info[i].ion_fd = -1;
    }
    return 0;
}

static int alloc_ion_mem(unsigned int size)
{
    int result = 0, i = 0;
    struct ion_fd_data fd_data;
    struct ion_allocation_data ionAllocData;

    ionAllocData.flags = 0;
    ionAllocData.len = size;
    ionAllocData.align = sysconf(_SC_PAGESIZE);
    ionAllocData.heap_mask =
            ION_HEAP(ION_IOMMU_HEAP_ID) |
            ION_HEAP(ION_SYSTEM_HEAP_ID);

    for (i = 0; i < NUM_BUFFERS; i++) {
        mem_info[i].ion_fd = open("/dev/ion", O_RDWR|O_DSYNC);
        if (mem_info[i].ion_fd < 0) {
            perror("ERROR: Can't open ion ");
            return -errno;
        }

        LOGV("%s: ion_fd=%d\n", __func__, mem_info[i].ion_fd);

        result = ioctl(mem_info[i].ion_fd, ION_IOC_ALLOC,  &ionAllocData);
        if(result){
            perror("ION_IOC_ALLOC Failed ");
            close(mem_info[i].ion_fd);
            return result;
        }

        fd_data.handle = ionAllocData.handle;
        mem_info[i].handle_data.handle = ionAllocData.handle;
        result = ioctl(mem_info[i].ion_fd, ION_IOC_MAP, &fd_data);
        if (result) {
            perror("ION_IOC_MAP Failed ");
            free_ion_mem();
            return result;
        }
        mem_info[i].mem_buf = (unsigned char *)mmap(NULL, size, PROT_READ |
                    PROT_WRITE, MAP_SHARED, fd_data.fd, 0);
        mem_info[i].mem_fd = fd_data.fd;

        if (mem_info[i].mem_buf == MAP_FAILED) {
            perror("ERROR: ION MAP_FAILED ");
            mem_info[i].mem_buf = NULL;
            free_ion_mem();
            return -ENOMEM;
        }

        sem_init(&(mem_info[i].acquire), 0, 0);
        sem_init(&(mem_info[i].retire),  0, 1);

        LOGV("%s: ion_fd=%d mem_fd=%d buf=%p\n", __func__, mem_info[i].ion_fd,
                mem_info[i].mem_fd, mem_info[i].mem_buf);
        mem_info[i].offset = 0;
    }

    return 0;
}

static int allocate_overlay(int fd)
{
    int ret = 0;

    if (!isDisplaySplit()) {
        // Check if overlay is already allocated
        if (MSMFB_NEW_REQUEST == overlayL_id) {
            struct mdp_overlay overlayL;

            memset(&overlayL, 0 , sizeof (struct mdp_overlay));

            /* Fill Overlay Data */
            overlayL.src.width  = ALIGN(gr_draw.width, 32);
            overlayL.src.height = gr_draw.height;
            overlayL.src.format = map_mdp_pixel_format();
            overlayL.src_rect.w = gr_draw.width;
            overlayL.src_rect.h = gr_draw.height;
            overlayL.dst_rect.w = gr_draw.width;
            overlayL.dst_rect.h = gr_draw.height;
            overlayL.alpha = 0xFF;
            overlayL.transp_mask = MDP_TRANSP_NOP;
            overlayL.id = MSMFB_NEW_REQUEST;
            ret = ioctl(fd, MSMFB_OVERLAY_SET, &overlayL);
            if (ret < 0) {
                perror("Overlay Set Failed \n");
                return ret;
            }
            overlayL_id = overlayL.id;
        }
    } else {
        float xres = getFbXres();
        int lSplit = getLeftSplit();
        float lSplitRatio = lSplit / xres;
        float lCropWidth = gr_draw.width * lSplitRatio;
        int lWidth = lSplit;
        int rWidth = gr_draw.width - lSplit;
        int height = gr_draw.height;

        if (MSMFB_NEW_REQUEST == overlayL_id) {

            struct mdp_overlay overlayL;

            memset(&overlayL, 0 , sizeof (struct mdp_overlay));

            /* Fill OverlayL Data */
            overlayL.src.width  = ALIGN(gr_draw.width, 32);
            overlayL.src.height = gr_draw.height;
            overlayL.src.format = map_mdp_pixel_format();
            overlayL.src_rect.x = 0;
            overlayL.src_rect.y = 0;
            overlayL.src_rect.w = lCropWidth;
            overlayL.src_rect.h = gr_draw.height;
            overlayL.dst_rect.x = 0;
            overlayL.dst_rect.y = 0;
            overlayL.dst_rect.w = lWidth;
            overlayL.dst_rect.h = height;
            overlayL.alpha = 0xFF;
            overlayL.transp_mask = MDP_TRANSP_NOP;
            overlayL.id = MSMFB_NEW_REQUEST;
            ret = ioctl(fd, MSMFB_OVERLAY_SET, &overlayL);
            if (ret < 0) {
                perror("OverlayL Set Failed");
                return ret;
            }
            overlayL_id = overlayL.id;
        }
        if (MSMFB_NEW_REQUEST == overlayR_id) {
            struct mdp_overlay overlayR;

            memset(&overlayR, 0 , sizeof (struct mdp_overlay));

            /* Fill OverlayR Data */
            overlayR.src.width  = ALIGN(gr_draw.width, 32);
            overlayR.src.height = gr_draw.height;
            overlayR.src.format = map_mdp_pixel_format();
            overlayR.src_rect.x = lCropWidth;
            overlayR.src_rect.y = 0;
            overlayR.src_rect.w = gr_draw.width - lCropWidth;
            overlayR.src_rect.h = gr_draw.height;
            overlayR.dst_rect.x = 0;
            overlayR.dst_rect.y = 0;
            overlayR.dst_rect.w = rWidth;
            overlayR.dst_rect.h = height;
            overlayR.alpha = 0xFF;
            overlayR.flags = MDSS_MDP_RIGHT_MIXER;
            overlayR.transp_mask = MDP_TRANSP_NOP;
            overlayR.id = MSMFB_NEW_REQUEST;
            ret = ioctl(fd, MSMFB_OVERLAY_SET, &overlayR);
            if (ret < 0) {
                perror("OverlayR Set Failed");
                return ret;
            }
            overlayR_id = overlayR.id;
        }
    }

    return 0;
}

static int free_overlay(int fd)
{
    int ret = 0;
    struct mdp_display_commit ext_commit;

    if (!isDisplaySplit()) {
        if (overlayL_id != MSMFB_NEW_REQUEST) {
            ret = ioctl(fd, MSMFB_OVERLAY_UNSET, &overlayL_id);
            if (ret) {
                perror("Overlay Unset Failed");
                overlayL_id = MSMFB_NEW_REQUEST;
                return ret;
            }
        }
    } else {

        if (overlayL_id != MSMFB_NEW_REQUEST) {
            ret = ioctl(fd, MSMFB_OVERLAY_UNSET, &overlayL_id);
            if (ret) {
                perror("OverlayL Unset Failed");
                overlayL_id = MSMFB_NEW_REQUEST;
                return ret;
            }
        }

        if (overlayR_id != MSMFB_NEW_REQUEST) {
            ret = ioctl(fd, MSMFB_OVERLAY_UNSET, &overlayR_id);
            if (ret) {
                perror("OverlayR Unset Failed");
                overlayR_id = MSMFB_NEW_REQUEST;
                return ret;
            }
        }
    }
    memset(&ext_commit, 0, sizeof(struct mdp_display_commit));
    ext_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
    ret = ioctl(fd, MSMFB_DISPLAY_COMMIT, &ext_commit);
    if (ret < 0) {
        perror("ERROR: Clear MSMFB_DISPLAY_COMMIT failed!");
        overlayL_id = MSMFB_NEW_REQUEST;
        overlayR_id = MSMFB_NEW_REQUEST;
        return ret;
    }
    overlayL_id = MSMFB_NEW_REQUEST;
    overlayR_id = MSMFB_NEW_REQUEST;

    return 0;
}

static int overlay_display_frame(int num)
{
    int ret = 0;
    struct msmfb_overlay_data ovdataL, ovdataR;
    struct mdp_display_commit ext_commit;
    size_t size = gr_draw.row_bytes * gr_draw.height;

    if (!isDisplaySplit()) {
        if (overlayL_id == MSMFB_NEW_REQUEST) {
            perror("display_frame failed, no overlay\n");
            return -EINVAL;
        }

        memset(&ovdataL, 0, sizeof(struct msmfb_overlay_data));

        ovdataL.id = overlayL_id;
        ovdataL.data.flags = 0;
        ovdataL.data.offset = mem_info[num].offset;
        ovdataL.data.memory_id = mem_info[num].mem_fd;

        ret = ioctl(fb_fd, MSMFB_OVERLAY_PLAY, &ovdataL);
        if (ret < 0) {
            perror("overlay_display_frame failed, overlay play Failed\n");
            return ret;
        }
    } else {

        if (overlayL_id == MSMFB_NEW_REQUEST) {
            perror("display_frame failed, no overlayL \n");
            return -EINVAL;
        }

        memset(&ovdataL, 0, sizeof(struct msmfb_overlay_data));

        ovdataL.id = overlayL_id;
        ovdataL.data.flags = 0;
        ovdataL.data.offset = mem_info[num].offset;
        ovdataL.data.memory_id = mem_info[num].mem_fd;

        ret = ioctl(fb_fd, MSMFB_OVERLAY_PLAY, &ovdataL);
        if (ret < 0) {
            perror("overlay_display_frame failed, overlayL play Failed\n");
            return ret;
        }

        if (overlayR_id == MSMFB_NEW_REQUEST) {
            perror("display_frame failed, no overlayR \n");
            return -EINVAL;
        }

        memset(&ovdataR, 0, sizeof(struct msmfb_overlay_data));

        ovdataR.id = overlayR_id;
        ovdataR.data.flags = 0;
        ovdataR.data.offset = mem_info[num].offset;
        ovdataR.data.memory_id = mem_info[num].mem_fd;
        ret = ioctl(fb_fd, MSMFB_OVERLAY_PLAY, &ovdataR);
        if (ret < 0) {
            perror("overlay_display_frame failed, overlayR play Failed\n");
            return ret;
        }
    }

    memset(&ext_commit, 0, sizeof(struct mdp_display_commit));
    ext_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
    wait_for_vsync();
    ret = ioctl(fb_fd, MSMFB_DISPLAY_COMMIT, &ext_commit);
    if (ret < 0) {
        perror("overlay_display_frame failed, overlay commit Failed\n!");
        return ret;
    }

    return ret;
}

static void *overlay_commit_thread(void *data)
{
    int frame = 0;
    int prev = NUM_BUFFERS - 1;

    while (true) {

        LOGV("%s: wait acquire frame=%d\n", __func__, frame);
        sem_wait(&(mem_info[frame].acquire));

        if (overlay_display_frame(frame) < 0) {
            // Free and allocate overlay in failure case
            // so that next cycle can be retried
            free_overlay(fb_fd);
            allocate_overlay(fb_fd);
        }

        prev = frame - 1;
        if (prev < 0)
            prev = NUM_BUFFERS - 1;

        // retire the last buffer
        LOGV("%s: post retire frame=%d\n", __func__, frame);
        sem_post(&(mem_info[prev].retire));

        frame++;
        if (frame >= NUM_BUFFERS)
            frame = 0;

    }
    return NULL;
}

static gr_surface overlay_init(minui_backend* backend)
{
    int fd;
    void *bits = NULL;
    pthread_t commit_thread;

    struct fb_fix_screeninfo fi;

    fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd < 0) {
        perror("cannot open fb0");
        return NULL;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return NULL;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return NULL;
    }

    if (isTargetMdp5())
        setDisplaySplit();

    LOGI("fb0 reports (possibly inaccurate):\n"
           "  vi.bits_per_pixel = %d\n"
           "  vi.red.offset   = %3d   .length = %3d\n"
           "  vi.green.offset = %3d   .length = %3d\n"
           "  vi.blue.offset  = %3d   .length = %3d\n",
           vi.bits_per_pixel,
           vi.red.offset, vi.red.length,
           vi.green.offset, vi.green.length,
           vi.blue.offset, vi.blue.length);

    fi.line_length = ALIGN(vi.xres, 32) * PIXEL_SIZE;

    gr_draw.width = vi.xres;
    gr_draw.height = vi.yres;
    gr_draw.row_bytes = fi.line_length;
    gr_draw.pixel_bytes = vi.bits_per_pixel / 8;

    fb_fd = fd;

    LOGI("overlay: %d (%d x %d)\n", fb_fd, gr_draw.width, gr_draw.height);

    overlay_blank(backend, true);
    overlay_blank(backend, false);

    if (alloc_ion_mem(fi.line_length * vi.yres) || allocate_overlay(fb_fd)) {
        free_ion_mem();
        return NULL;
    }

    vsync_init(fd);

    pthread_create(&commit_thread, NULL, overlay_commit_thread, NULL);

    gr_draw.data = mem_info[0].mem_buf;

    return &gr_draw;
}

static int next_buf = 0;

static gr_surface overlay_flip(minui_backend* backend __unused)
{
    LOGV("%s: post acquire next_buf=%d\n", __func__, next_buf);

    // post the active buffer
    sem_post(&(mem_info[next_buf].acquire));

    LOGV("%s: wait retire next_buf=%d\n", __func__, next_buf);
    sem_wait(&(mem_info[next_buf].retire));

    // wait for the next buffer
    next_buf++;
    if (next_buf >= NUM_BUFFERS)
        next_buf = 0;

    gr_draw.data = mem_info[next_buf].mem_buf;
    return &gr_draw;
}

static void overlay_blank(minui_backend* backend __unused, bool blank) {
    if (blank)
        free_overlay(fb_fd);

    ioctl(fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);

    if (!blank)
        allocate_overlay(fb_fd);
}

static void overlay_exit(minui_backend* backend __unused) {
    free_overlay(fb_fd);
    free_ion_mem();
    close(fb_fd);
    fb_fd = -1;
}

#else

bool target_has_overlay()
{
    return false;
}

minui_backend* open_overlay() {
    return NULL;
}

#endif
